#include "runtime/vm.h"

#include "bytecode/emitter.h"
#include "bytecode/format.h"
#include "frontend/ast/expr.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "package/package.h"
#include "runtime/macro_expander.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "vm test failed: " << message << "\n";
    std::exit(1);
  }
}

template <typename Predicate>
bool wait_for_condition(Predicate predicate,
                        std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

std::size_t count_occurrences(const std::string &text,
                              const std::string &needle) {
  if (needle.empty()) {
    return 0;
  }
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

amber::bytecode::EmitResult emit_ok(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }
  return emit_result;
}

const amber::bytecode::BcMethod *
method_by_name(const amber::bytecode::BcModule &module,
               const std::string &name);
std::uint32_t symbol_id_or_die(const amber::bytecode::BcModule &module,
                               const std::string &name);
std::uint32_t append_integer_const(amber::bytecode::BcModule *module,
                                   std::int64_t value);
const amber::runtime::ExecutionLocal *
execution_local_by_name(const amber::runtime::ExecutionResult &result,
                        const std::string &name);
std::string
string_value_text_or_die(const amber::runtime::Value &value,
                         const amber::bytecode::BcModule &module,
                         const amber::runtime::ExecutionResult &result);
amber::runtime::ExecutionResult execute_emitted_init(const std::string &source);

amber::runtime::RuntimeIoProviderStatus provider_ok() {
  amber::runtime::RuntimeIoProviderStatus status;
  status.handled = true;
  status.ok = true;
  return status;
}

class TestIoProvider final : public amber::runtime::RuntimeIoProvider {
public:
  std::unordered_map<std::string, std::string> files;

  amber::runtime::RuntimeIoProviderStatus
  fs_exists(const std::string &path) override {
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    status.boolean = files.find(path) != files.end();
    return status;
  }

  amber::runtime::RuntimeIoProviderStatus
  fs_file(const std::string &path) override {
    return fs_exists(path);
  }

  amber::runtime::RuntimeIoProviderStatus
  fs_dir(const std::string &path) override {
    (void)path;
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    status.boolean = false;
    return status;
  }

  amber::runtime::RuntimeIoProviderStatus
  fs_metadata(const std::string &path) override {
    auto found = files.find(path);
    if (found == files.end()) {
      amber::runtime::RuntimeIoProviderStatus status = provider_ok();
      status.ok = false;
      status.error_name = "FileNotFoundError";
      status.message = "provider file not found";
      return status;
    }
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    status.size = found->second.size();
    status.file = true;
    return status;
  }

  amber::runtime::RuntimeIoProviderStatus
  fs_read_bytes(const std::string &path,
                std::optional<std::size_t> limit) override {
    auto found = files.find(path);
    if (found == files.end()) {
      amber::runtime::RuntimeIoProviderStatus status = provider_ok();
      status.ok = false;
      status.error_name = "FileNotFoundError";
      status.message = "provider file not found";
      return status;
    }
    if (limit.has_value() && found->second.size() > *limit) {
      amber::runtime::RuntimeIoProviderStatus status = provider_ok();
      status.ok = false;
      status.error_name = "ArgumentError";
      status.message = "read_bytes limit exceeded";
      return status;
    }
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    status.bytes = found->second;
    status.count = status.bytes.size();
    return status;
  }

  amber::runtime::RuntimeIoProviderStatus
  fs_write_bytes(const std::string &path, const std::string &bytes, bool create,
                 bool truncate, bool append = false) override {
    (void)create;
    amber::runtime::RuntimeIoProviderStatus status = provider_ok();
    if (append && !truncate) {
      files[path] += bytes;
    } else {
      files[path] = bytes;
    }
    status.count = bytes.size();
    return status;
  }
};

void test_execute_emitted_method() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def echo(x):\n"
                                                          "  x\n");
  expect(emit_result.module.methods.size() == 1, "expected one method");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.methods[0].entry_code_id,
      {amber::runtime::Value::integer(7)});
  expect(exec.ok(), "echo execution failed");
  expect(exec.value.is_integer(), "echo should return integer");
  expect(exec.value.as_integer() == 7, "echo should return argument");
}

// Regression for bench/polyglot/repro/direct_method_capture_failure.am:
// executing a compiled top-level method directly by its entry_code_id must
// materialize the closure captures that module initialization would create.
// Before the fix this faulted with "VMError: capture slot out of range"
// because main()'s body captures the sibling helper() but direct entry never
// ran module init to bind it.
void test_direct_entry_materializes_sibling_captures() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def helper(value):\n"
                                                          "  value + 1\n"
                                                          "\n"
                                                          "def main():\n"
                                                          "  helper(41)\n"
                                                          "\n"
                                                          "main()\n");
  const amber::bytecode::BcMethod *main_method =
      method_by_name(emit_result.module, "main");
  expect(main_method != nullptr, "main method should exist");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, main_method->entry_code_id);
  expect(exec.ok(), "direct main entry should not fault on sibling captures");
  expect(exec.value.is_integer(), "direct main entry should return integer");
  expect(exec.value.as_integer() == 42,
         "direct main entry should return helper(41) == 42");
}

void test_execute_return_keyword() {
  const amber::runtime::ExecutionResult exec = execute_emitted_init(
      "def early(x):\n"
      "  if x > 10:\n"
      "    return \"big\"\n"
      "  \"small\"\n"
      "\n"
      "def bare(x):\n"
      "  x * 2\n"
      "  return\n"
      "\n"
      "def from_loop(n):\n"
      "  i = 0\n"
      "  while i < 100:\n"
      "    if i == n:\n"
      "      return i * 10\n"
      "    i = i + 1\n"
      "  0 - 1\n"
      "\n"
      "a = early(20)\n"
      "b = early(3)\n"
      "c = bare(21)\n"
      "d = from_loop(4)\n"
      "if a == \"big\" and b == \"small\" and c == 42 and d == 40:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect(exec.ok(), "return keyword probe should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "return keyword probe result");
}

void test_execute_return_runs_nested_ensure_inner_to_outer() {
  const amber::runtime::ExecutionResult exec =
      execute_emitted_init("cell = [\"\"]\n"
                           "\n"
                           "def record(t):\n"
                           "  cell[0] = cell[0] + t\n"
                           "\n"
                           "def inner(v):\n"
                           "  try:\n"
                           "    try:\n"
                           "      if v > 0:\n"
                           "        return \"deep\"\n"
                           "      \"shallow\"\n"
                           "    ensure:\n"
                           "      record(\"I\")\n"
                           "  ensure:\n"
                           "    record(\"O\")\n"
                           "\n"
                           "r = inner(1)\n"
                           "r + \":\" + cell[0]\n");
  expect(exec.ok(), "return-through-ensure probe should execute");
  const std::string text =
      string_value_text_or_die(exec.value, amber::bytecode::BcModule{}, exec);
  expect(text == "deep:IO",
         "return must run nested ensure bodies inner-to-outer");
}

void test_execute_zero_division_error() {
  const amber::runtime::ExecutionResult fault_exec =
      execute_emitted_init("1 / 0\n");
  expect(!fault_exec.ok() && fault_exec.fault.has_value() &&
             fault_exec.fault->error_name == "ZeroDivisionError",
         "unhandled division by zero should fault as ZeroDivisionError");

  const amber::runtime::ExecutionResult rescued =
      execute_emitted_init("def f():\n"
                           "  try:\n"
                           "    1 / 0\n"
                           "  rescue ZeroDivisionError |e|:\n"
                           "    e.message()\n"
                           "\n"
                           "f()\n");
  expect(rescued.ok(), "rescued division by zero should execute");
  const std::string text = string_value_text_or_die(
      rescued.value, amber::bytecode::BcModule{}, rescued);
  expect(text == "division by zero",
         "ZeroDivisionError should be rescuable with message");

  const amber::runtime::ExecutionResult modulo =
      execute_emitted_init("7 % 0\n");
  expect(!modulo.ok() && modulo.fault.has_value() &&
             modulo.fault->error_name == "ZeroDivisionError",
         "modulo by zero should fault as ZeroDivisionError");
}

void test_execute_checked_int_overflow() {
  const amber::runtime::ExecutionResult add_overflow =
      execute_emitted_init("9223372036854775807 + 1\n");
  expect(!add_overflow.ok() && add_overflow.fault.has_value() &&
             add_overflow.fault->error_name == "OverflowError",
         "checked Int64 add overflow should raise OverflowError");

  const amber::runtime::ExecutionResult mul_overflow =
      execute_emitted_init("4611686018427387904 * 4\n");
  expect(!mul_overflow.ok() && mul_overflow.fault.has_value() &&
             mul_overflow.fault->error_name == "OverflowError",
         "checked Int64 mul overflow should raise OverflowError");

  const amber::runtime::ExecutionResult div_overflow =
      execute_emitted_init("a = 0 - 9223372036854775807\n"
                           "b = a - 1\n"
                           "b / (0 - 1)\n");
  expect(!div_overflow.ok() && div_overflow.fault.has_value() &&
             div_overflow.fault->error_name == "OverflowError",
         "INT64_MIN / -1 should raise OverflowError");

  const amber::runtime::ExecutionResult shl_overflow =
      execute_emitted_init("1 << 63\n");
  expect(!shl_overflow.ok() && shl_overflow.fault.has_value() &&
             shl_overflow.fault->error_name == "OverflowError",
         "checked Int64 shift-left overflow should raise OverflowError");

  const amber::runtime::ExecutionResult pow_overflow =
      execute_emitted_init("3 ** 64\n");
  expect(!pow_overflow.ok() && pow_overflow.fault.has_value() &&
             pow_overflow.fault->error_name == "OverflowError",
         "checked Int64 pow overflow should raise OverflowError");

  const amber::runtime::ExecutionResult rescued =
      execute_emitted_init("def f():\n"
                           "  try:\n"
                           "    9223372036854775807 + 1\n"
                           "  rescue OverflowError |e|:\n"
                           "    \"caught: \" + e.message()\n"
                           "\n"
                           "f()\n");
  expect(rescued.ok(), "rescued overflow should execute");
  const std::string text = string_value_text_or_die(
      rescued.value, amber::bytecode::BcModule{}, rescued);
  expect(text == "caught: Int overflow in `+`",
         "OverflowError should be rescuable");

  const amber::runtime::ExecutionResult in_loop =
      execute_emitted_init("x = 9223372036854775800\n"
                           "i = 0\n"
                           "while i < 100:\n"
                           "  x = x + 1\n"
                           "  i = i + 1\n"
                           "x\n");
  expect(!in_loop.ok() && in_loop.fault.has_value() &&
             in_loop.fault->error_name == "OverflowError",
         "quickened loop add overflow should raise OverflowError");
}

void test_execute_numeric_profile_modes() {
  const amber::runtime::ExecutionResult wrap =
      execute_emitted_init("numeric:\n"
                           "  int: Int64\n"
                           "  overflow: wrapping\n"
                           "\n"
                           "9223372036854775807 + 1\n");
  expect(wrap.ok(), "wrapping profile should execute");
  expect(wrap.value.is_integer() &&
             wrap.value.as_integer() ==
                 std::numeric_limits<std::int64_t>::min(),
         "wrapping Int64 overflow wraps to INT64_MIN");

  const amber::runtime::ExecutionResult saturate =
      execute_emitted_init("numeric:\n"
                           "  int: Int8\n"
                           "  overflow: saturating\n"
                           "\n"
                           "a = 100 + 100\n"
                           "b = 0 - 100 - 100\n"
                           "a * 3 + b\n");
  expect(saturate.ok(), "saturating profile should execute");
  expect(saturate.value.is_integer() && saturate.value.as_integer() == -1,
         "saturating Int8: a==127, b==-128, 127*3 clamps to 127, "
         "127 + -128 == -1");

  const amber::runtime::ExecutionResult narrow_checked =
      execute_emitted_init("numeric:\n"
                           "  int: Int8\n"
                           "\n"
                           "100 + 100\n");
  expect(!narrow_checked.ok() && narrow_checked.fault.has_value() &&
             narrow_checked.fault->error_name == "OverflowError",
         "checked Int8 overflow raises OverflowError");
}

void test_execute_big_int_explicit_type() {
  const amber::runtime::ExecutionResult exec =
      execute_emitted_init("big = BigInt(9223372036854775807)\n"
                           "square = big * big\n"
                           "mixed = 5 + BigInt(10)\n"
                           "back = BigInt(42).to_int()\n"
                           "\"#{square}:#{mixed}:#{back}\"\n");
  expect(exec.ok(), "BigInt probe should execute");
  const std::string text =
      string_value_text_or_die(exec.value, amber::bytecode::BcModule{}, exec);
  expect(text == "85070591730234615847396907784232501249:15:42",
         "explicit BigInt arithmetic is exact and mixes with Int");

  const amber::runtime::ExecutionResult to_int_overflow = execute_emitted_init(
      "BigInt(\"123456789012345678901234567890\").to_int()\n");
  expect(!to_int_overflow.ok() && to_int_overflow.fault.has_value() &&
             to_int_overflow.fault->error_name == "OverflowError",
         "BigInt#to_int out of Int range raises OverflowError");

  const amber::runtime::ExecutionResult div_zero =
      execute_emitted_init("BigInt(1) / BigInt(0)\n");
  expect(!div_zero.ok() && div_zero.fault.has_value() &&
             div_zero.fault->error_name == "ZeroDivisionError",
         "BigInt division by zero raises ZeroDivisionError");
}

void test_execute_mixed_sign_division_semantics() {
  const amber::runtime::ExecutionResult exec =
      execute_emitted_init("a = 7 / (0 - 2)\n"
                           "b = 7 % (0 - 2)\n"
                           "c = 7 // (0 - 2)\n"
                           "d = (0 - 7) / 2\n"
                           "e = (0 - 7) % 2\n"
                           "f = (0 - 7) // 2\n"
                           "\"#{a}:#{b}:#{c}:#{d}:#{e}:#{f}\"\n");
  expect(exec.ok(), "mixed-sign division probe should execute");
  const std::string text =
      string_value_text_or_die(exec.value, amber::bytecode::BcModule{}, exec);
  expect(text == "-3:-1:-4:-3:1:-4",
         "`/` truncates while `%` and `//` floor (pinned semantics)");
}

void test_execute_module_init_calls_top_level_def() {
  amber::bytecode::EmitResult emit_result = emit_ok("def f(x):\n"
                                                    "  x + 42\n"
                                                    "\n"
                                                    "f(3)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  expect(decoded.module.init.has_entry_code_id,
         "module init entry should exist");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "module init top-level def call failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 45,
         "module init should return top-level function result");
}

void test_execute_native_range_literal() {
  amber::bytecode::EmitResult emit_result = emit_ok("(0..5).array\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "native Range prelude literal execution failed");
  expect(exec.value.is_list(), "native Range literal should materialize");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> items =
      exec.value.as_list();
  expect(items != nullptr && items->items.size() == 6,
         "native Range literal materialized item count");
  for (std::size_t i = 0; i < items->items.size(); ++i) {
    expect(items->items[i].is_integer() &&
               items->items[i].as_integer() == static_cast<std::int64_t>(i),
           "native Range literal materialized item");
  }

  emit_result = emit_ok("(5..1:-2).array\n");
  const amber::runtime::ExecutionResult descending =
      amber::runtime::execute_code(emit_result.module,
                                   emit_result.module.init.entry_code_id);
  expect(descending.ok(), "native descending Range literal execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      descending_items = descending.value.as_list();
  expect(descending_items != nullptr && descending_items->items.size() == 3,
         "native descending Range materialized item count");
  expect(descending_items->items[0].as_integer() == 5 &&
             descending_items->items[1].as_integer() == 3 &&
             descending_items->items[2].as_integer() == 1,
         "native descending Range materialized values");

  emit_result = emit_ok("(0..5).lazy()\n");
  const amber::runtime::ExecutionResult lazy = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(lazy.ok(), "native Range lazy conversion failed");
  expect(amber::runtime::value_to_debug_string(lazy.value, &emit_result.module)
                 .find("Range") == std::string::npos,
         "native LazySeq should not be classified as a Range");
}

void test_execute_emitted_collection_literals() {
  amber::bytecode::EmitResult list_result = emit_ok("[1, 2 + 3]\n");
  amber::runtime::ExecutionResult list_exec = amber::runtime::execute_code(
      list_result.module, list_result.module.init.entry_code_id);
  expect(list_exec.ok(), "list literal execution failed");
  expect(list_exec.value.is_list(), "list literal should return list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      list_exec.value.as_list();
  expect(list != nullptr && list->items.size() == 2, "list literal item count");
  expect(list->items[0].is_integer() && list->items[0].as_integer() == 1,
         "list literal first value");
  expect(list->items[1].is_integer() && list->items[1].as_integer() == 5,
         "list literal evaluates nested expression");

  amber::bytecode::EmitResult tuple_result = emit_ok("(1, 2)\n");
  amber::runtime::ExecutionResult tuple_exec = amber::runtime::execute_code(
      tuple_result.module, tuple_result.module.init.entry_code_id);
  expect(tuple_exec.ok(), "tuple literal execution failed");
  expect(tuple_exec.value.is_tuple(), "tuple literal should return tuple");
  const amber::runtime::IntrusivePtr<amber::runtime::TupleValue> tuple =
      tuple_exec.value.as_tuple();
  expect(tuple != nullptr && tuple->items.size() == 2,
         "tuple literal item count");
  expect(tuple->items[1].is_integer() && tuple->items[1].as_integer() == 2,
         "tuple literal second value");

  amber::bytecode::EmitResult set_result = emit_ok("{1, 1, 2}\n");
  const amber::bytecode::DecodeResult set_decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(set_result.module));
  expect(set_decoded.ok(),
         amber::bytecode::verify_errors_to_json(set_decoded.errors));
  amber::runtime::ExecutionResult set_exec = amber::runtime::execute_code(
      set_decoded.module, set_decoded.module.init.entry_code_id);
  expect(set_exec.ok(), "set literal execution failed");
  expect(set_exec.value.is_set(), "set literal should return set");
  const amber::runtime::IntrusivePtr<amber::runtime::SetValue> set =
      set_exec.value.as_set();
  expect(set != nullptr && set->items.size() == 2,
         "set literal collapses duplicate values");
  expect(set->items[0].is_integer() && set->items[0].as_integer() == 1,
         "set literal first value");
  expect(set->items[1].is_integer() && set->items[1].as_integer() == 2,
         "set literal second value");

  amber::bytecode::EmitResult map_result =
      emit_ok("{id: 1, id: 2, \"name\": :ok}\n");
  amber::runtime::ExecutionResult map_exec = amber::runtime::execute_code(
      map_result.module, map_result.module.init.entry_code_id);
  expect(map_exec.ok(), "map literal execution failed");
  expect(map_exec.value.is_map(), "map literal should return map");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> map =
      map_exec.value.as_map();
  expect(map != nullptr && map->entries.size() == 2,
         "duplicate map key is replaced");
  expect(map->entries[0].symbol_id == symbol_id_or_die(map_result.module, "id"),
         "map id key");
  expect(map->entries[0].value.is_integer() &&
             map->entries[0].value.as_integer() == 2,
         "later duplicate map key wins");
  expect(map->entries[1].key.is_string() &&
             string_value_text_or_die(map->entries[1].key, map_result.module,
                                      map_exec) == "name",
         "string map key remains Str key");
  expect(map->entries[1].value.is_symbol() &&
             map->entries[1].value.as_symbol().symbol_id ==
                 symbol_id_or_die(map_result.module, "ok"),
         "symbol literal map value");

  amber::bytecode::EmitResult same_name_map_result =
      emit_ok("x = 42\n"
              "{a: 100500, x:}\n");
  amber::runtime::ExecutionResult same_name_map_exec =
      amber::runtime::execute_code(
          same_name_map_result.module,
          same_name_map_result.module.init.entry_code_id);
  expect(same_name_map_exec.ok(), "same-name map literal execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> same_name_map =
      same_name_map_exec.value.as_map();
  expect(same_name_map != nullptr && same_name_map->entries.size() == 2,
         "same-name map entry count");
  expect(same_name_map->entries[1].symbol_id ==
             symbol_id_or_die(same_name_map_result.module, "x"),
         "same-name map key");
  expect(same_name_map->entries[1].value.is_integer() &&
             same_name_map->entries[1].value.as_integer() == 42,
         "same-name map reads local value");

  amber::bytecode::EmitResult index_store_result =
      emit_ok("items = [1, 2, 3]\n"
              "items[1] = 9\n"
              "lookup = {answer: 1}\n"
              "lookup[:answer] = items[1] + 1\n"
              "[items[0], items[1], lookup[:answer]]\n");
  amber::runtime::ExecutionResult index_store_exec =
      amber::runtime::execute_code(
          index_store_result.module,
          index_store_result.module.init.entry_code_id);
  expect(index_store_exec.ok(), "index assignment execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      index_store_items = index_store_exec.value.as_list();
  expect(index_store_items != nullptr && index_store_items->items.size() == 3,
         "index assignment result shape");
  expect(index_store_items->items[0].as_integer() == 1 &&
             index_store_items->items[1].as_integer() == 9 &&
             index_store_items->items[2].as_integer() == 10,
         "index assignment mutates list and map");

  amber::bytecode::EmitResult size_property_result =
      emit_ok("items = [1, 2, 3]\n"
              "lookup = {a: 1, b: 2}\n"
              "[items.count, items.length, items.size, items.count(), "
              "items.length(), items.size(), lookup.count, lookup.length, "
              "lookup.size]\n");
  amber::runtime::ExecutionResult size_property_exec =
      amber::runtime::execute_code(
          size_property_result.module,
          size_property_result.module.init.entry_code_id);
  expect(size_property_exec.ok(), "collection size property execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> size_parts =
      size_property_exec.value.as_list();
  expect(size_parts != nullptr && size_parts->items.size() == 9,
         "collection size property result shape");
  for (const amber::runtime::Value &value : size_parts->items) {
    expect(value.is_integer() && value.as_integer() >= 2 &&
               value.as_integer() <= 3,
           "collection size property part");
  }
  for (std::size_t i = 0; i < 6; ++i) {
    expect(size_parts->items[i].as_integer() == 3,
           "sequence count/length/size property value");
  }
  for (std::size_t i = 6; i < 9; ++i) {
    expect(size_parts->items[i].as_integer() == 2,
           "map count/length/size property value");
  }

  amber::bytecode::EmitResult member_op_result =
      emit_ok("items = [1, 2, 3]\n"
              "lookup = {alpha: 1, beta: 2}\n"
              "[2 in items, items.contains?(2), items.include?(2), "
              "items.member?(2), items.includes?(2), 4 in items, "
              ":alpha in lookup, lookup.contains?(:alpha), "
              "lookup.include?(:alpha), lookup.member?(:alpha), "
              "lookup.includes?(:alpha), :missing in lookup]\n");
  amber::runtime::ExecutionResult member_op_exec = amber::runtime::execute_code(
      member_op_result.module, member_op_result.module.init.entry_code_id);
  expect(member_op_exec.ok(), "member op execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> member_parts =
      member_op_exec.value.as_list();
  expect(member_parts != nullptr && member_parts->items.size() == 12,
         "member op result shape");
  for (std::size_t i = 0; i < member_parts->items.size(); ++i) {
    expect(member_parts->items[i].is_bool(), "member op part should be bool");
  }
  for (std::size_t i = 0; i < 5; ++i) {
    expect(member_parts->items[i].as_bool(),
           "sequence member op aliases should agree");
  }
  expect(!member_parts->items[5].as_bool(),
         "sequence member op should reject absent value");
  for (std::size_t i = 6; i < 11; ++i) {
    expect(member_parts->items[i].as_bool(),
           "map member op aliases should agree");
  }
  expect(!member_parts->items[11].as_bool(),
         "map member op should reject absent key");

  amber::bytecode::EmitResult inline_if_result =
      emit_ok("if false then 1 else 2\n");
  amber::runtime::ExecutionResult inline_if_exec = amber::runtime::execute_code(
      inline_if_result.module, inline_if_result.module.init.entry_code_id);
  expect(inline_if_exec.ok(), "inline conditional execution failed");
  expect(inline_if_exec.value.is_integer() &&
             inline_if_exec.value.as_integer() == 2,
         "inline conditional returns selected branch");

  amber::bytecode::EmitResult conditional_list_result =
      emit_ok("x = 0\n"
              "[1, (x = 1) if false, 3, x]\n");
  amber::runtime::ExecutionResult conditional_list_exec =
      amber::runtime::execute_code(
          conditional_list_result.module,
          conditional_list_result.module.init.entry_code_id);
  expect(conditional_list_exec.ok(),
         "conditional list literal execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      conditional_list = conditional_list_exec.value.as_list();
  expect(conditional_list != nullptr && conditional_list->items.size() == 3,
         "conditional list skips false element");
  expect(conditional_list->items[0].is_integer() &&
             conditional_list->items[0].as_integer() == 1,
         "conditional list first value");
  expect(conditional_list->items[1].is_integer() &&
             conditional_list->items[1].as_integer() == 3,
         "conditional list keeps following value");
  expect(conditional_list->items[2].is_integer() &&
             conditional_list->items[2].as_integer() == 0,
         "conditional list does not evaluate skipped value");

  amber::bytecode::EmitResult conditional_set_result =
      emit_ok("{1, 2 if false, 3 unless false}\n");
  amber::runtime::ExecutionResult conditional_set_exec =
      amber::runtime::execute_code(
          conditional_set_result.module,
          conditional_set_result.module.init.entry_code_id);
  expect(conditional_set_exec.ok(), "conditional set literal execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::SetValue> conditional_set =
      conditional_set_exec.value.as_set();
  expect(conditional_set != nullptr && conditional_set->items.size() == 2,
         "conditional set applies if and unless");

  amber::bytecode::EmitResult conditional_map_result =
      emit_ok("x = 0\n"
              "[x, {a: 1, b: (x = 1) if false, c: 3}]\n");
  amber::runtime::ExecutionResult conditional_map_exec =
      amber::runtime::execute_code(
          conditional_map_result.module,
          conditional_map_result.module.init.entry_code_id);
  expect(conditional_map_exec.ok(), "conditional map literal execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      conditional_map_pair = conditional_map_exec.value.as_list();
  expect(conditional_map_pair != nullptr &&
             conditional_map_pair->items.size() == 2,
         "conditional map result pair");
  expect(conditional_map_pair->items[0].is_integer() &&
             conditional_map_pair->items[0].as_integer() == 0,
         "conditional map does not evaluate skipped value");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> conditional_map =
      conditional_map_pair->items[1].as_map();
  expect(conditional_map != nullptr && conditional_map->entries.size() == 2,
         "conditional map skips false entry");
  expect(conditional_map->entries[0].symbol_id ==
             symbol_id_or_die(conditional_map_result.module, "a"),
         "conditional map first key");
  expect(conditional_map->entries[1].symbol_id ==
             symbol_id_or_die(conditional_map_result.module, "c"),
         "conditional map keeps later key");
}

void test_execute_emitted_v20_7_spread() {
  amber::bytecode::EmitResult emit_result = emit_ok("def collect(a, b, c):\n"
                                                    "  [a, b, c]\n"
                                                    "\n"
                                                    "def probe():\n"
                                                    "  args = [2, 3]\n"
                                                    "  collect(1, *args)\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "positional spread probe method exists");
  amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "positional call spread execution failed");
  expect(exec.value.is_list(), "positional call spread returns list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> call_items =
      exec.value.as_list();
  expect(call_items != nullptr && call_items->items.size() == 3,
         "positional call spread result count");
  expect(call_items->items[0].as_integer() == 1 &&
             call_items->items[1].as_integer() == 2 &&
             call_items->items[2].as_integer() == 3,
         "positional call spread result values");

  emit_result = emit_ok("class Config:\n"
                        "  class_method def build(x, alpha:, beta: 2):\n"
                        "    x + alpha + beta\n"
                        "\n"
                        "def probe():\n"
                        "  opts = {alpha: 5, beta: 6}\n"
                        "  Config.build(4, **opts)\n");
  method = method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "keyword spread probe method exists");
  exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "keyword call spread execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 15,
         "keyword call spread binds keyword entries");

  emit_result = emit_ok("class Config:\n"
                        "  class_method def build(x, alpha:, beta: 2):\n"
                        "    x + alpha + beta\n"
                        "\n"
                        "def probe():\n"
                        "  opts = {\"alpha\": 5, \"beta\": 6}\n"
                        "  Config.build(4, **opts)\n");
  method = method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "keyword string spread probe method exists");
  exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "keyword string-key spread execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 15,
         "keyword spread converts string keys to keyword names");

  emit_result = emit_ok("class Config:\n"
                        "  class_method def pack(a, b:, x:, y:):\n"
                        "    [a, b, x, y]\n"
                        "\n"
                        "def probe():\n"
                        "  b = 5\n"
                        "  x = 42\n"
                        "  y = 6\n"
                        "  paren = Config.pack(1, b:, x:, y:)\n"
                        "  bare = Config.pack 1, b:, x:, y:\n"
                        "  [paren[0], paren[1], paren[2], bare[2], bare[3]]\n");
  method = method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "same-name keyword probe method exists");
  exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "same-name keyword call execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      same_name_call_items = exec.value.as_list();
  expect(same_name_call_items != nullptr &&
             same_name_call_items->items.size() == 5,
         "same-name keyword result count");
  expect(same_name_call_items->items[0].as_integer() == 1 &&
             same_name_call_items->items[1].as_integer() == 5 &&
             same_name_call_items->items[2].as_integer() == 42 &&
             same_name_call_items->items[3].as_integer() == 42 &&
             same_name_call_items->items[4].as_integer() == 6,
         "same-name keyword args read locals");

  emit_result = emit_ok("def pack(a, b:, x:, y:):\n"
                        "  [a, b, x, y]\n"
                        "\n"
                        "def probe():\n"
                        "  b = 5\n"
                        "  x = 42\n"
                        "  y = 6\n"
                        "  paren = pack(1, b:, x:, y:)\n"
                        "  bare = pack 1, b:, x:, y:\n"
                        "  [paren[0], paren[1], paren[2], bare[2], bare[3]]\n");
  method = method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "same-name closure keyword probe method exists");
  exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "same-name closure keyword call execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      same_name_closure_items = exec.value.as_list();
  expect(same_name_closure_items != nullptr &&
             same_name_closure_items->items.size() == 5,
         "same-name closure keyword result count");
  expect(same_name_closure_items->items[0].as_integer() == 1 &&
             same_name_closure_items->items[1].as_integer() == 5 &&
             same_name_closure_items->items[2].as_integer() == 42 &&
             same_name_closure_items->items[3].as_integer() == 42 &&
             same_name_closure_items->items[4].as_integer() == 6,
         "same-name closure keyword args read locals");

  emit_result = emit_ok("[1, *[2, 3], *(4..5)]\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "array spread execution failed");
  expect(exec.value.is_list(), "array spread returns list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> array_items =
      exec.value.as_list();
  expect(array_items != nullptr && array_items->items.size() == 5,
         "array spread item count");
  for (std::size_t i = 0; i < array_items->items.size(); ++i) {
    expect(array_items->items[i].is_integer() &&
               array_items->items[i].as_integer() ==
                   static_cast<std::int64_t>(i + 1U),
           "array spread item value");
  }

  emit_result = emit_ok("{1, *[1, 2, 3]}\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "set spread execution failed");
  expect(exec.value.is_set(), "set spread returns set");
  const amber::runtime::IntrusivePtr<amber::runtime::SetValue> set_items =
      exec.value.as_set();
  expect(set_items != nullptr && set_items->items.size() == 3,
         "set spread collapses duplicates");

  emit_result = emit_ok("{a: 1, **{a: 2, b: 3}}\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "map spread execution failed");
  expect(exec.value.is_map(), "map spread returns map");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> map_items =
      exec.value.as_map();
  expect(map_items != nullptr && map_items->entries.size() == 2,
         "map spread entry count");
  expect(map_items->entries[0].symbol_id ==
             symbol_id_or_die(emit_result.module, "a"),
         "map spread keeps first key order");
  expect(map_items->entries[0].value.as_integer() == 2,
         "map spread overwrites duplicate key");
  expect(map_items->entries[1].symbol_id ==
             symbol_id_or_die(emit_result.module, "b"),
         "map spread appends new key");

  emit_result = emit_ok("[*(1..)]\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "InfiniteCollectionError",
         "open-ended range spread is rejected");

  emit_result = emit_ok("class Config:\n"
                        "  class_method def build(alpha:):\n"
                        "    alpha\n"
                        "\n"
                        "def probe():\n"
                        "  opts = {\"bad name\": 5}\n"
                        "  Config.build(**opts)\n");
  method = method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "keyword spread invalid key probe exists");
  exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "KeywordArgumentError",
         "keyword spread rejects non-convertible string keys");

  emit_result = emit_ok("class Config:\n"
                        "  class_method def build(alpha:):\n"
                        "    alpha\n"
                        "\n"
                        "def probe():\n"
                        "  opts = {\"alpha\": 5}\n"
                        "  Config.build(alpha: 1, **opts)\n");
  method = method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "keyword spread duplicate probe exists");
  exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "KeywordArgumentError",
         "keyword spread rejects duplicate converted keys");
}

void test_execute_emitted_v20_6_value_keyed_maps() {
  const amber::runtime::ExecutionResult exec = execute_emitted_init(
      "k = \"dyn\"\n"
      "m = {1: 10, 1.0: 20, \"name\": 30, (k): 40, [1, 2]: 50}\n"
      "s = {[1, 2], (1, 2), 1, 1.0}\n"
      "tm = Map{\"typed\": 7}\n"
      "ts = Set{1, 1.0, [1, 2], (1, 2)}\n"
      "r = {(1..3): 60}\n"
      // v20.7/v20.8: ordinary maps are name-indifferent, so a `:name` symbol
      // probe now resolves the string key "name" (was null under exact-key).
      "if m[1] == 20 and m[1.0] == 20 and m[\"name\"] == 30 and "
      "m[?:name] == 30 and m[k] == 40 and m[(1, 2)] == 50 and "
      "m[[1, 2]] == 50 and m.keys().count() == 4 and s.count() == 2 and "
      "tm[\"typed\"] == 7 and ts.count() == 2 and r[(1..3)] == 60:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect(exec.ok(), "v20.6 value-keyed map probe should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "v20.6 value-keyed map probe result");
}

void test_top_level_function_closure_captures_sibling_function() {
  amber::bytecode::EmitResult emit_result = emit_ok("def tap(x):\n"
                                                    "  x\n"
                                                    "\n"
                                                    "def describe(a):\n"
                                                    "  tap(a)\n"
                                                    "\n"
                                                    "describe(7)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "top-level function sibling capture failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "top-level function should call captured sibling function");
}

void test_direct_top_level_method_entry_materializes_module_captures() {
  amber::bytecode::EmitResult emit_result = emit_ok("def helper(value):\n"
                                                    "  value + 1\n"
                                                    "\n"
                                                    "def main():\n"
                                                    "  helper(41)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::bytecode::BcMethod *main_method =
      method_by_name(decoded.module, "main");
  expect(main_method != nullptr, "direct entry main method exists");
  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(decoded.module, main_method->entry_code_id);
  expect(exec.ok(),
         "direct top-level method entry should materialize module captures");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "direct top-level method entry should call captured sibling helper");

  amber::runtime::RuntimeWorld world(decoded.module);
  const amber::runtime::ExecutionResult init =
      world.execute(decoded.module.init.entry_code_id);
  expect(init.ok(), "module init should persist top-level bindings");
  const amber::runtime::ExecutionResult after_init =
      world.execute(main_method->entry_code_id);
  expect(after_init.ok(),
         "direct top-level method entry should reuse persisted captures");
  expect(after_init.value.is_integer() && after_init.value.as_integer() == 42,
         "direct top-level method after init should call sibling helper");
}

void test_top_level_function_self_recursion() {
  amber::bytecode::EmitResult emit_result = emit_ok("def fact(n):\n"
                                                    "  if n == 0:\n"
                                                    "    1\n"
                                                    "  else:\n"
                                                    "    n * fact(n - 1)\n"
                                                    "\n"
                                                    "fact(5)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "top-level recursive function failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 120,
         "top-level recursive function should return factorial");
}

void test_top_level_clause_function_self_recursion() {
  amber::bytecode::EmitResult emit_result =
      emit_ok("def fact(0): 1\n"
              "def fact(n) if n > 0: n * fact(n - 1)\n"
              "\n"
              "fact(5)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "top-level recursive clause function failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 120,
         "top-level recursive clause function should return factorial");
}

void test_top_level_plain_def_fallback_clause_recursion() {
  amber::bytecode::EmitResult emit_result = emit_ok("def frac(x):\n"
                                                    "  x * frac(x - 1)\n"
                                                    "\n"
                                                    "def frac(2):\n"
                                                    "  2\n"
                                                    "\n"
                                                    "frac(5)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "plain def fallback clause recursion failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 120,
         "plain def fallback clause recursion should return factorial");
}

void test_top_level_guarded_clause_function_self_recursion() {
  amber::bytecode::EmitResult emit_result = emit_ok("def frac(x) if x > 0:\n"
                                                    "  x * frac(x - 1)\n"
                                                    "\n"
                                                    "def frac(0): 1\n"
                                                    "\n"
                                                    "frac(5)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "guarded recursive clause function failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 120,
         "guarded recursive clause function should return factorial");
}

void test_top_level_clause_function_self_recursion_base_one_orders() {
  amber::bytecode::EmitResult recursive_first =
      emit_ok("def fact(x) if x > 1: x * fact(x - 1)\n"
              "def fact(1): 1\n"
              "\n"
              "fact(5)\n");
  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(recursive_first.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(exec.ok(), "recursive-first base-one clause recursion failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 120,
         "recursive-first base-one clause recursion should return factorial");

  amber::bytecode::EmitResult base_first =
      emit_ok("def fact(1): 1\n"
              "def fact(x) if x > 1: x * fact(x - 1)\n"
              "\n"
              "fact(5)\n");
  decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(base_first.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  exec = amber::runtime::execute_code(decoded.module,
                                      decoded.module.init.entry_code_id);
  expect(exec.ok(), "base-first base-one clause recursion failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 120,
         "base-first base-one clause recursion should return factorial");
}

void test_top_level_clause_function_miss_raises_match_error() {
  amber::bytecode::EmitResult emit_result =
      emit_ok("def fact(x) if x > 1: x * fact(x - 1)\n"
              "def fact(0): 1\n"
              "\n"
              "fact(5)\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      decoded.module, decoded.module.init.entry_code_id);
  expect(!exec.ok(), "uncovered clause recursion should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "MatchError",
         "uncovered clause recursion should raise MatchError");
}

void test_runtime_capability_checks() {
  amber::bytecode::BcModule module;
  module.capabilities.push_back(
      amber::capability::make_capability("fs.read", "./data"));

  amber::runtime::RuntimeWorldOptions options;
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.read", "./data"));
  amber::runtime::RuntimeWorld world(module, std::move(options));

  const amber::runtime::RuntimeCapabilityCheckResult allowed =
      world.check_capability("fs.read", "./data/orders.csv");
  expect(allowed.ok, "runtime capability should allow granted path");
  const amber::runtime::RuntimeCapabilityCheckResult denied =
      world.check_capability("fs.read", "./private/orders.csv");
  expect(!denied.ok && denied.error_name == "CapabilityError",
         "runtime capability should deny ungranted path");
  const amber::runtime::RuntimeCapabilityResolution resolution =
      world.capability_resolution();
  expect(resolution.ok, "runtime capability resolution should be satisfied");

  amber::runtime::RuntimeWorld denied_world(module);
  const amber::runtime::RuntimeCapabilityCheckResult missing =
      denied_world.check_capability("fs.read", "./data/orders.csv");
  expect(!missing.ok && missing.error_name == "CapabilityError",
         "default runtime world should deny requested host resources");
}

void test_runtime_effect_checks() {
  amber::bytecode::BcModule module;
  module.effects.push_back(amber::effect::make_effect_summary(
      "clocky", "function", {"time"}, {"time"}, true));

  amber::runtime::RuntimeWorldOptions options;
  options.enforce_effects = true;
  options.allowed_effects = {"time"};
  amber::runtime::RuntimeWorld world(module, options);
  const amber::runtime::RuntimeEffectValidation validation =
      world.effect_validation();
  expect(validation.ok, "matching declared/observed effects should validate");

  const amber::runtime::RuntimeEffectCheckResult allowed =
      world.check_effects({"time"});
  expect(allowed.ok, "runtime effect allowance should accept time");
  const amber::runtime::RuntimeEffectCheckResult denied =
      world.check_effects({"fs"});
  expect(!denied.ok && denied.error_name == "EffectViolationError",
         "runtime effect allowance should reject fs");

  amber::bytecode::BcModule mismatch;
  mismatch.effects.push_back(amber::effect::make_effect_summary(
      "bad", "function", {}, {"time"}, true));
  amber::runtime::RuntimeWorld mismatch_world(mismatch);
  const amber::runtime::RuntimeEffectValidation mismatch_validation =
      mismatch_world.effect_validation();
  expect(!mismatch_validation.ok,
         "runtime effect validation should catch row mismatch");
}

void test_runtime_replay_trace_recording_and_divergence() {
  amber::bytecode::BcModule module;
  module.capabilities.push_back(
      amber::capability::make_capability("fs.read", "./data"));

  amber::runtime::RuntimeWorldOptions options;
  options.record_replay_trace = true;
  options.trace_id = "trace-test";
  options.virtual_time_start = 10;
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.read", "./data"));
  amber::runtime::RuntimeWorld world(module, options);

  expect(world.check_capability("fs.read", "./data/orders.csv").ok,
         "replay trace setup capability should pass");
  expect(world.check_effects({"time"}).ok,
         "replay trace setup effect check should pass");
  expect(world.freeze_world().ok(), "replay trace setup freeze should pass");

  const amber::runtime::RuntimeReplayTrace trace = world.replay_trace();
  expect(trace.events.size() == 4, "runtime trace should record four events");
  expect(trace.events[0].name == "loader.module.load" &&
             trace.events[1].name == "capability.check" &&
             trace.events[2].name == "effect.boundary" &&
             trace.events[3].name == "world.freeze",
         "runtime trace event order should be deterministic");
  expect(trace.events[0].timestamp_or_virtual_time == 10 &&
             trace.events[3].timestamp_or_virtual_time == 13,
         "runtime trace should use deterministic virtual time");

  const std::string serialized = amber::replay::serialize_trace(trace);
  const amber::replay::ReplayTraceParseResult parsed =
      amber::replay::parse_trace(serialized);
  expect(parsed.ok(), "serialized runtime replay trace should parse");
  expect(amber::replay::compare_traces(trace, parsed.trace).ok,
         "parsed runtime trace should match original");

  amber::runtime::RuntimeWorldOptions replay_options = options;
  replay_options.enforce_replay = true;
  replay_options.expected_replay = trace;
  amber::runtime::RuntimeWorld replay_world(module, replay_options);
  replay_world.check_capability("fs.read", "./data/orders.csv");
  replay_world.check_effects({"time"});
  replay_world.freeze_world();
  expect(replay_world.replay_validation().ok,
         "matching runtime replay should validate");

  amber::runtime::RuntimeWorld diverged_world(module, replay_options);
  diverged_world.check_effects({"time"});
  const amber::runtime::RuntimeReplayValidation diverged =
      diverged_world.replay_validation();
  expect(!diverged.ok && !diverged.diagnostics.empty() &&
             diverged.diagnostics[0].error_name == "ReplayDivergenceError",
         "divergent runtime replay should report ReplayDivergenceError");
}

void test_runtime_schema_and_table_metadata() {
  amber::bytecode::BcModule module;
  amber::data::SchemaDefinition order_v1;
  order_v1.name = "Order";
  order_v1.version = 1;
  order_v1.fields.push_back({"id",
                             "integer",
                             true,
                             false,
                             {},
                             amber::data::kSchemaFieldFlagPrimaryKey});
  order_v1.fields.push_back({"amount", "float", true, false, {}, 0});
  amber::data::SchemaDefinition order_v2 = order_v1;
  order_v2.version = 2;
  order_v2.fields.push_back({"status", "string", false, false, "new", 0});
  module.schemas = {order_v1, order_v2};
  module.schema_migrations.push_back(
      {"Order", 1, 2, "compatible",
       amber::data::kSchemaMigrationFlagCompatible});
  amber::data::TablePlan plan;
  plan.plan_id = "orders.high_value";
  plan.op = "filter";
  plan.input_refs = {"orders"};
  plan.arguments = {"amount > 100"};
  plan.column_dependencies = {{"orders", "amount"}};
  plan.flags = amber::data::kTablePlanFlagLazy;
  module.table_plans.push_back(plan);

  amber::runtime::RuntimeWorld world(module);
  expect(world.schema_validation().ok,
         "runtime schema metadata should validate");
  expect(world.table_plan_validation().ok,
         "runtime table metadata should validate");
  expect(world.table_plan_validation().plans.size() == 1,
         "runtime table plan should be exposed");
  expect(amber::data::table_plan_fingerprint(
             world.table_plan_validation().plans[0])
                 .size() == 64,
         "runtime table plan fingerprint should be sha256 hex");

  const amber::runtime::RuntimePackageMirror mirror = world.package_mirror();
  expect(mirror.schemas.size() == 2 && mirror.schema_migrations.size() == 1 &&
             mirror.table_plans.size() == 1,
         "runtime mirror should expose W11.4 metadata");

  amber::bytecode::BcModule invalid = module;
  invalid.schemas[1].fields.back().required = true;
  invalid.schemas[1].fields.back().default_value.clear();
  amber::runtime::RuntimeWorld invalid_world(invalid);
  expect(!invalid_world.schema_validation().ok,
         "runtime schema validation should reject incompatible migration");
}

void test_runtime_wasm_and_accelerator_metadata() {
  amber::bytecode::BcModule module;

  amber::wasm_accel::WasmInterfaceEntry import_entry;
  import_entry.name = "fs.read";
  import_entry.kind = "resource";
  import_entry.type_signature = "resource";
  import_entry.capability =
      amber::capability::make_capability("fs.read", "./data");

  amber::wasm_accel::WasmInterfaceEntry export_entry;
  export_entry.name = "normalize";
  export_entry.kind = "func";
  export_entry.type_signature = "(Order) -> Order";
  export_entry.schema_name = "Order";

  amber::wasm_accel::WasmComponent component;
  component.name = "analytics.plugin";
  component.world = "analytics-plugin";
  component.flags = amber::wasm_accel::kWasmComponentFlagFrozenWorld |
                    amber::wasm_accel::kWasmComponentFlagRawFfiDenied |
                    amber::wasm_accel::kWasmComponentFlagWorldMutationDenied;
  component.imports.push_back(import_entry);
  component.exports.push_back(export_entry);
  module.wasm_components.push_back(component);

  amber::wasm_accel::AcceleratorKernel kernel;
  kernel.kernel_id = "scale.f32";
  kernel.entry = "scale";
  kernel.target = "gpu";
  kernel.effect_row = {"gpu"};
  kernel.params.push_back({"xs", "Tensor[F32]", "device", 0});
  kernel.params.push_back({"factor", "F32", "scalar", 0});
  module.accelerator_kernels.push_back(kernel);

  amber::runtime::RuntimeWorld world(module);
  expect(world.wasm_validation().ok, "runtime wasm metadata should validate");
  expect(world.accelerator_validation().ok,
         "runtime accelerator metadata should validate");

  const amber::runtime::RuntimePackageMirror mirror = world.package_mirror();
  expect(mirror.wasm_components.size() == 1 &&
             mirror.accelerator_kernels.size() == 1,
         "runtime mirror should expose W11.5 metadata");

  amber::bytecode::BcModule invalid = module;
  invalid.accelerator_kernels[0].forbidden_features.push_back(
      "dynamic_dispatch");
  amber::runtime::RuntimeWorld invalid_world(invalid);
  expect(!invalid_world.accelerator_validation().ok,
         "runtime accelerator validation should reject dynamic dispatch");
}

void test_runtime_modern_profile_metadata() {
  amber::bytecode::BcModule module;

  amber::modern::AgentSymbol symbol;
  symbol.symbol_id = "main::compute";
  symbol.name = "compute";
  symbol.kind = "function";
  symbol.module = "main";
  symbol.visibility = "public";
  module.agent_symbols.push_back(symbol);

  amber::modern::ContractSpec contract;
  contract.owner = "Account.withdraw";
  contract.kind = "require";
  contract.expression = "amount > 0";
  module.contracts.push_back(contract);

  module.privacy_labels.push_back({"pii", "pii", 0});
  module.privacy_policies.push_back(
      {"PrivateAudit", "redact", "pii", {}, 0, 0});
  amber::modern::LineageNode lineage;
  lineage.node_id = "transform.users";
  lineage.kind = "transform";
  lineage.output = "users.redacted";
  lineage.labels = {"pii"};
  module.lineage_nodes.push_back(lineage);

  amber::modern::WorkflowStep step;
  step.workflow = "ImportOrders";
  step.name = "commit";
  step.effect_row = {"db"};
  step.idempotency_key = "batch-1";
  module.workflow_steps.push_back(step);

  amber::runtime::RuntimeWorld world(module);
  expect(world.agent_validation().ok, "runtime agent metadata should validate");
  expect(world.contract_validation().ok,
         "runtime contract metadata should validate");
  expect(world.privacy_validation().ok,
         "runtime privacy metadata should validate");
  expect(world.workflow_validation().ok,
         "runtime workflow metadata should validate");

  const amber::runtime::RuntimePackageMirror mirror = world.package_mirror();
  expect(mirror.agent_symbols.size() == 1 && mirror.contracts.size() == 1 &&
             mirror.privacy_labels.size() == 1 &&
             mirror.workflow_steps.size() == 1,
         "runtime mirror should expose W11.6 metadata");

  amber::bytecode::BcModule invalid = module;
  invalid.workflow_steps[0].name = "";
  amber::runtime::RuntimeWorld invalid_world(invalid);
  expect(!invalid_world.workflow_validation().ok,
         "runtime workflow validation should reject malformed steps");
}

void test_branching_and_last_result() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def flag(x):\n"
                                                          "  if x:\n"
                                                          "    1\n"
                                                          "  else:\n"
                                                          "    0\n");
  expect(emit_result.module.methods.size() == 1, "expected one flag method");

  const std::uint32_t code_id = emit_result.module.methods[0].entry_code_id;
  const amber::runtime::ExecutionResult when_true =
      amber::runtime::execute_code(emit_result.module, code_id,
                                   {amber::runtime::Value::boolean(true)});
  expect(when_true.ok(), "flag(true) execution failed");
  expect(when_true.value.is_integer() && when_true.value.as_integer() == 1,
         "flag(true) should return 1");

  const amber::runtime::ExecutionResult when_false =
      amber::runtime::execute_code(emit_result.module, code_id,
                                   {amber::runtime::Value::boolean(false)});
  expect(when_false.ok(), "flag(false) execution failed");
  expect(when_false.value.is_integer() && when_false.value.as_integer() == 0,
         "flag(false) should return 0");

  const amber::runtime::ExecutionResult when_null =
      amber::runtime::execute_code(emit_result.module, code_id,
                                   {amber::runtime::Value::null()});
  expect(when_null.ok(), "flag(null) execution failed");
  expect(when_null.value.is_integer() && when_null.value.as_integer() == 0,
         "flag(null) should treat null as falsey");
}

void test_manual_closure_call_and_capture() {
  using namespace amber::bytecode;

  BcModule module;
  Constant five;
  five.kind = ConstantKind::Integer;
  five.int_value = 5;
  module.const_pool.push_back(five);

  BcCode outer;
  outer.code_id = 1;
  outer.kind = CodeKind::Method;
  outer.reg_count = 3;
  outer.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  outer.instructions.push_back(
      {Opcode::MakeClosure,
       {{1, false}, {2, false}, {1, false}, {0, false}, {0, false}}});
  outer.instructions.push_back({Opcode::Call,
                                {{2, false},
                                 {1, false},
                                 {0, false},
                                 {0, false},
                                 {-1, true},
                                 {0, false}}});
  outer.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode inner;
  inner.code_id = 2;
  inner.kind = CodeKind::Block;
  inner.reg_count = 1;
  inner.instructions.push_back({Opcode::LoadUpval, {{0, false}, {0, false}}});
  inner.instructions.push_back({Opcode::Return, {{0, false}}});

  module.code_objects = {outer, inner};

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "manual closure execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "manual closure should return captured integer");
}

void test_runtime_uninitialized_register_read_raises_name_error() {
  using namespace amber::bytecode;

  BcModule module;
  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back({Opcode::Move, {{1, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(!exec.ok(), "uninitialized register read should fail");
  expect(exec.fault->error_name == "NameError",
         "uninitialized register read should surface NameError");
}

void test_runtime_string_interpolation_and_conversions() {
  amber::bytecode::EmitResult emit_result =
      emit_ok("(\"#{1 + 2}\" == \"3\") and "
              "(\"#{null}\" == \"null\") and "
              "(\"#{true}\" == \"true\") and "
              "(\"#{false}\" == \"false\") and "
              "(\"#{1 + 2} #{$_}\" == \"3 3\") and "
              "(\"123\".to_int() == 123) and "
              "(123.to_str() == \"123\") and "
              "(5.6.to_str() == \"5.6\") and "
              "(\"true\".to_bool() == true) and "
              "(\"false\".to_bool() == false) and "
              "(42.str == \"42\") and "
              "(\"123\".int == 123) and "
              "(\"3.5\".float == 3.5) and "
              "(\"true\".bool == true) and "
              "(\"answer\".symbol == :answer) and "
              "((1, 2).array == (1, 2).to_array()) and "
              "([1, 2].tuple == [1, 2].to_tuple()) and "
              "([1, 2].set == [1, 2].to_set()) and "
              "([(:answer, 42)].map == [(:answer, 42)].to_map()) and "
              "(Int(\"123\") == 123) and "
              "(Str(42) == \"42\") and "
              "(Amber.stringify(123, mode: :display) == \"123\")\n");
  expect(emit_result.module.init.has_entry_code_id,
         "conversion/interpolation module init exists");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "conversion/interpolation execution failed");
  expect(exec.value.is_bool() && exec.value.as_bool(),
         "conversion/interpolation predicates should all hold");

  emit_result = emit_ok("5.6.to_str()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "float to_str execution failed");
  expect(amber::runtime::value_to_debug_string(
             exec.value, &emit_result.module, &exec.runtime_strings,
             &exec.runtime_symbols) == "\"5.6\"",
         "runtime-created to_str result should remain printable");

  emit_result = emit_ok("\"fresh-runtime-symbol\".symbol\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "symbol property conversion execution failed");
  expect(amber::runtime::value_to_debug_string(
             exec.value, &emit_result.module, &exec.runtime_strings,
             &exec.runtime_symbols) == ":fresh-runtime-symbol",
         "runtime-created symbol result should remain printable");

  emit_result = emit_ok("\"abc\".to_int()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "ValueError",
         "invalid integer string should raise ValueError");

  emit_result = emit_ok("[].to_int()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "TypeError",
         "unsupported to_int source should raise TypeError");

  emit_result = emit_ok("6.str()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "NoMethodError",
         "conversion property aliases should not become ordinary methods");
}

void test_runtime_text_output_helpers_and_io_sinks() {
  amber::bytecode::EmitResult emit_result = emit_ok("print \"hello\"\n"
                                                    "x = p \"debug\"\n"
                                                    "pp([1, 2])\n"
                                                    "x\n");
  std::shared_ptr<amber::runtime::RuntimeTextWriter> stdout_buffer =
      amber::runtime::RuntimeTextWriter::buffer();
  {
    amber::runtime::RuntimeOutputScope scope(stdout_buffer, {});
    const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
        emit_result.module, emit_result.module.init.entry_code_id);
    expect(exec.ok(), "print/p/pp command-form execution failed");
    expect(exec.value.is_string(), "p should return its single argument");
  }
  const std::string output = stdout_buffer->to_string();
  expect(output.find("hello\n\"debug\"\n") == 0U,
         "print and p should write display/inspect lines");
  expect(output.find("[\n  1,\n  2,\n]") != std::string::npos,
         "pp should write structured pretty output");

  emit_result = emit_ok("print()\n");
  stdout_buffer = amber::runtime::RuntimeTextWriter::buffer();
  {
    amber::runtime::RuntimeOutputScope scope(stdout_buffer, {});
    const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
        emit_result.module, emit_result.module.init.entry_code_id);
    expect(exec.ok() && exec.value.is_null(), "print() should return null");
  }
  expect(stdout_buffer->to_string() == "\n",
         "print() should write one newline");

  emit_result = emit_ok("print(\"a\", \"b\")\n");
  stdout_buffer = amber::runtime::RuntimeTextWriter::buffer();
  {
    amber::runtime::RuntimeOutputScope scope(stdout_buffer, {});
    const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
        emit_result.module, emit_result.module.init.entry_code_id);
    expect(exec.ok() && exec.value.is_null(),
           "multi-value print should return null");
  }
  expect(stdout_buffer->to_string() == "a\nb\n",
         "multi-value print should write each argument on its own line");

  emit_result = emit_ok("p()\n");
  stdout_buffer = amber::runtime::RuntimeTextWriter::buffer();
  {
    amber::runtime::RuntimeOutputScope scope(stdout_buffer, {});
    const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
        emit_result.module, emit_result.module.init.entry_code_id);
    expect(exec.ok() && exec.value.is_null(), "p() should return null");
  }
  expect(stdout_buffer->to_string().empty(), "p() should write nothing");

  emit_result = emit_ok("p(1, 2)\n");
  stdout_buffer = amber::runtime::RuntimeTextWriter::buffer();
  {
    amber::runtime::RuntimeOutputScope scope(stdout_buffer, {});
    const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
        emit_result.module, emit_result.module.init.entry_code_id);
    expect(exec.ok() && exec.value.is_tuple(),
           "multi-value p should return tuple");
    expect(exec.value.as_tuple()->items.size() == 2,
           "multi-value p tuple shape");
  }
  expect(stdout_buffer->to_string() == "1\n2\n",
         "multi-value p should write each argument on its own line");

  emit_result = emit_ok("buffer = io.Buffer.new()\n"
                        "p(\"x\", to: buffer)\n"
                        "buffer.to_str()\n");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "explicit buffer sink execution failed");
  expect(string_value_text_or_die(exec.value, emit_result.module, exec) ==
             "\"x\"\n",
         "explicit buffer sink should receive p output");

  emit_result = emit_ok("buffer = io.Buffer.new()\n"
                        "io.with_output(stdout: buffer): print \"x\"\n"
                        "io.with_output(stdout: buffer): p \"y\"\n"
                        "buffer.to_str()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "dynamic output scope execution failed");
  expect(string_value_text_or_die(exec.value, emit_result.module, exec) ==
             "x\n\"y\"\n",
         "io.with_output should rebind logical stdout");

  emit_result = emit_ok("buffer = io.Buffer.new()\n"
                        "try:\n"
                        "  io.with_output(stdout: buffer):\n"
                        "    print \"before\"\n"
                        "    raise ValueError(\"scoped\")\n"
                        "  \"no error\"\n"
                        "rescue ValueError |e|:\n"
                        "  \"#{e.message()}:#{buffer.to_str()}\"\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "dynamic output scope rescue execution failed");
  expect(string_value_text_or_die(exec.value, emit_result.module, exec) ==
             "scoped:before\n",
         "io.with_output block exception should rescue outside scope");

  emit_result = emit_ok("buffer = io.Buffer.new()\n"
                        "handle = io.with_output(stdout: buffer): "
                        "task.spawn: print \"async\"\n"
                        "handle.wait()\n"
                        "buffer.to_str()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "task output inheritance execution failed");
  expect(string_value_text_or_die(exec.value, emit_result.module, exec) ==
             "async\n",
         "task.spawn should inherit logical stdout");

  emit_result = emit_ok("Kernel.p(\"warning\", to: io.stderr())\n");
  std::shared_ptr<amber::runtime::RuntimeTextWriter> stderr_buffer =
      amber::runtime::RuntimeTextWriter::buffer();
  {
    amber::runtime::RuntimeOutputScope scope({}, stderr_buffer);
    exec = amber::runtime::execute_code(emit_result.module,
                                        emit_result.module.init.entry_code_id);
    expect(exec.ok() && exec.value.is_string(),
           "explicit stderr p should return argument");
  }
  expect(stderr_buffer->to_string() == "\"warning\"\n",
         "explicit stderr sink should use current logical stderr");

  emit_result = emit_ok("buffer = io.Buffer.new()\n"
                        "buffer.close()\n"
                        "print(\"x\", to: buffer)\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "ClosedResourceError",
         "closed text writer should raise ClosedResourceError");

  emit_result = emit_ok("p(\"x\", to: 123)\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "TypeError",
         "invalid output sink should raise TypeError");

  emit_result = emit_ok("[Amber.stringify(\"hello\", mode: :inspect), "
                        "Amber.stringify([1, 2], mode: :pretty, "
                        "max_items: 1)]\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok() && exec.value.is_list(),
         "Amber.stringify inspect/pretty execution failed");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 2,
         "stringify result shape");
  expect(string_value_text_or_die(values->items[0], emit_result.module, exec) ==
             "\"hello\"",
         "inspect stringify should quote strings");
  expect(string_value_text_or_die(values->items[1], emit_result.module, exec)
                 .find("... 1 more") != std::string::npos,
         "pretty stringify should honor max_items");
}

void test_runtime_logger_source_surface_and_annotations() {
  amber::bytecode::EmitResult emit_result = emit_ok(
      "import task\n"
      "buffer = io.Buffer.new()\n"
      "color_buffer = io.Buffer.new()\n"
      "logger = io.Logger.new(to: buffer, level: :debug, color: false)\n"
      "logger.info(\"ready\")\n"
      "task.with_annotation(\"loader\"): logger.warn(\"slow\")\n"
      "logger.warning(\"again\")\n"
      "logger.debug(\"detail\")\n"
      "logger.log(:fatal, \"boom\")\n"
      "logger.flush()\n"
      "color_logger = io.Logger.new(to: color_buffer, level: :error, "
      "color: true)\n"
      "color_logger.debug(\"hidden\")\n"
      "color_logger.error(\"bad\")\n"
      "color_logger.flush()\n"
      "[buffer.to_str(), color_buffer.to_str()]\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "source-level logger execution failed");
  expect(exec.value.is_list(), "logger source result should be list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 2,
         "logger source result shape");
  const std::string plain =
      string_value_text_or_die(values->items[0], emit_result.module, exec);
  const std::string colored =
      string_value_text_or_die(values->items[1], emit_result.module, exec);

  expect(plain.find("[INFO] [thread=") != std::string::npos,
         "logger info record should include native thread id");
  expect(plain.find("[WARN] [loader thread=") != std::string::npos &&
             plain.find("slow\n") != std::string::npos,
         "logger warning should include scoped annotation and thread id");
  expect(plain.find("[WARN] [thread=") != std::string::npos &&
             plain.find("again\n") != std::string::npos,
         "logger warning alias should write unannotated record");
  expect(plain.find("[DEBUG] [thread=") != std::string::npos &&
             plain.find("detail\n") != std::string::npos,
         "logger debug should honor debug threshold");
  expect(plain.find("[FATAL] [thread=") != std::string::npos &&
             plain.find("boom\n") != std::string::npos,
         "logger fatal should be available");

  expect(colored.find("\033[31mERROR\033[0m") != std::string::npos,
         "forced color logger should emit xterm color codes");
  expect(colored.find("bad\n") != std::string::npos,
         "forced color logger should write enabled error");
  expect(colored.find("hidden") == std::string::npos,
         "logger level should filter disabled debug records");
}

void test_runtime_logger_parallel_native_threads_and_tasks() {
  const std::shared_ptr<amber::runtime::RuntimeTextWriter> buffer =
      amber::runtime::RuntimeTextWriter::buffer();
  amber::runtime::RuntimeLogger logger(
      buffer, amber::runtime::RuntimeLogLevel::Debug,
      amber::runtime::RuntimeLogColorMode::Never);

  constexpr int kThreads = 6;
  constexpr int kPerThread = 25;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&logger, thread_index]() {
      amber::runtime::RuntimeTaskAnnotationScope annotation(
          "native-" + std::to_string(thread_index));
      for (int message = 0; message < kPerThread; ++message) {
        const amber::runtime::RuntimeTextWriteResult result =
            logger.debug("message-" + std::to_string(thread_index) + "-" +
                         std::to_string(message));
        expect(result.ok, "native thread logger call should enqueue");
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  amber::runtime::RuntimeTaskModule task(3);
  amber::runtime::RuntimeTaskAnnotationScope parent_annotation("task-parent");
  std::vector<amber::runtime::RuntimeTaskHandle> handles;
  for (int index = 0; index < 4; ++index) {
    handles.push_back(task.spawn([&logger, index]() {
      logger.info("task-message-" + std::to_string(index));
      return amber::runtime::Value::null();
    }));
  }
  for (const amber::runtime::RuntimeTaskHandle &handle : handles) {
    const amber::runtime::RuntimeTaskPublicResult result =
        handle.wait(std::chrono::milliseconds(1000));
    expect(result.ok, "logger task should finish");
  }

  expect(logger.flush().ok, "logger flush should succeed");
  const std::string output = buffer->to_string();
  expect(count_occurrences(output, "[DEBUG] [native-") ==
             static_cast<std::size_t>(kThreads * kPerThread),
         "logger should keep all parallel native thread records");
  expect(output.find("message-0-0\n") != std::string::npos,
         "logger should retain native thread messages");
  expect(count_occurrences(output, "task-message-") == 4 &&
             output.find("[INFO] [task-parent task=") != std::string::npos &&
             output.find(" thread=") != std::string::npos,
         "logger tasks should include annotation, task id, and thread id");
}

void test_manual_call_invokes_object_call_method() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Callable", "call", "x"};
  module.strings = {"x", "param", "local"};

  BcClass callable;
  callable.class_name_sym_id = 0;
  callable.method_range_start = 0;
  callable.method_range_count = 1;
  module.classes.push_back(callable);

  BcMethod method;
  method.selector_sym_id = 1;
  method.entry_code_id = 2;
  method.flags = 1;
  method.params.push_back({2, 0, 0});
  module.methods.push_back(method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 3;
  caller.instructions.push_back({Opcode::Call,
                                 {{2, false},
                                  {0, false},
                                  {1, false},
                                  {1, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{2, false}}});
  caller.call_site_table.push_back({0, 0, 1, 0});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.local_layout.push_back({0, 0, 1, 2});
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1,
                                   {amber::runtime::Value::instance(instance),
                                    amber::runtime::Value::integer(7)});
  expect(exec.ok(), "object CALL dispatch failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "object CALL should dispatch to call method");
}

void test_execute_emitted_send_method() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def add(x, y):\n"
                                                          "  x + y\n");
  expect(emit_result.module.methods.size() == 1, "expected add method");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.methods[0].entry_code_id,
      {amber::runtime::Value::integer(9), amber::runtime::Value::integer(4)});
  expect(exec.ok(), "add execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 13,
         "add should return summed integer");
}

void test_execute_emitted_integer_specialized_ops() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("x = 10\n"
              "y = 3\n"
              "zero = 0\n"
              "neg = zero - x\n"
              "[x + y, x - 4, x * y, x / y, x % y, neg % y, x // y, "
              "neg // y, x < y, x > 4, x <= 10, x >= y, x == 10, "
              "x != y, x <=> y, y <=> x, y <=> 3, x & y, x | y, "
              "x ^ y, x << 2, x >> 1, 2 ** 10, 2 ** -1, 2.0 ** 3]\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "specialized integer ops execution failed");
  expect(exec.value.is_list(), "specialized integer ops should return list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 25,
         "specialized integer ops list size");
  expect(values->items[0].is_integer() && values->items[0].as_integer() == 13,
         "IADD should add integer registers");
  expect(values->items[1].is_integer() && values->items[1].as_integer() == 6,
         "ISUBK should subtract integer constant");
  expect(values->items[2].is_integer() && values->items[2].as_integer() == 30,
         "IMUL should multiply integer registers");
  expect(values->items[3].is_integer() && values->items[3].as_integer() == 3,
         "IDIV should divide integer registers");
  expect(values->items[4].is_integer() && values->items[4].as_integer() == 1,
         "IMOD should floor-mod positive integers");
  expect(values->items[5].is_integer() && values->items[5].as_integer() == 2,
         "IMOD should floor-mod negative integers");
  expect(values->items[6].is_integer() && values->items[6].as_integer() == 3,
         "IFLOORDIV should divide positive integers");
  expect(values->items[7].is_integer() && values->items[7].as_integer() == -4,
         "IFLOORDIV should floor negative quotients");
  expect(values->items[8].is_bool() && !values->items[8].as_bool(),
         "ILT should compare integer registers");
  expect(values->items[9].is_bool() && values->items[9].as_bool(),
         "IGTK should compare integer constant");
  expect(values->items[10].is_bool() && values->items[10].as_bool(),
         "ILEK should compare integer constant");
  expect(values->items[11].is_bool() && values->items[11].as_bool(),
         "IGE should compare integer registers");
  expect(values->items[12].is_bool() && values->items[12].as_bool(),
         "IEQK should compare integer constant");
  expect(values->items[13].is_bool() && values->items[13].as_bool(),
         "INE should compare integer registers");
  expect(values->items[14].is_integer() && values->items[14].as_integer() == 1,
         "ICMP should return 1 for greater lhs");
  expect(values->items[15].is_integer() && values->items[15].as_integer() == -1,
         "ICMP should return -1 for lesser lhs");
  expect(values->items[16].is_integer() && values->items[16].as_integer() == 0,
         "ICMPK should return 0 for equal operands");
  expect(values->items[17].is_integer() && values->items[17].as_integer() == 2,
         "IBITAND should and integer registers");
  expect(values->items[18].is_integer() && values->items[18].as_integer() == 11,
         "IBITOR should or integer registers");
  expect(values->items[19].is_integer() && values->items[19].as_integer() == 9,
         "IBITXOR should xor integer registers");
  expect(values->items[20].is_integer() && values->items[20].as_integer() == 40,
         "ISHLK should shift left");
  expect(values->items[21].is_integer() && values->items[21].as_integer() == 5,
         "ISHRK should shift right");
  expect(values->items[22].is_integer() &&
             values->items[22].as_integer() == 1024,
         "Int#** should return integer for non-negative exponent");
  expect(values->items[23].is_float() && values->items[23].as_float() == 0.5,
         "Int#** should return float for negative exponent");
  expect(values->items[24].is_float() && values->items[24].as_float() == 8.0,
         "Float#** should return float");
}

void test_execute_emitted_numeric_equality_and_new_ops() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("[1 == 1.0, 1 != 1.0, 7.5 % 2, 7.5 // 2, 1 <=> 1.0, "
              "1 < 2 < 3, 3 > 2 > 1, 1 < 2 < 2, 3 > 3 > 1]\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "numeric equality and new ops execution failed");
  expect(exec.value.is_list(), "numeric equality result should be list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 9,
         "numeric equality list size");
  expect(values->items[0].is_bool() && values->items[0].as_bool(),
         "int and float equality should compare numeric values");
  expect(values->items[1].is_bool() && !values->items[1].as_bool(),
         "int and float inequality should compare numeric values");
  expect(values->items[2].is_float() && values->items[2].as_float() == 1.5,
         "float modulo should use floor modulo");
  expect(values->items[3].is_float() && values->items[3].as_float() == 3.0,
         "float floor division should floor quotient");
  expect(values->items[4].is_integer() && values->items[4].as_integer() == 0,
         "mixed numeric spaceship should return zero");
  expect(values->items[5].is_bool() && values->items[5].as_bool(),
         "ascending comparison chain should pass");
  expect(values->items[6].is_bool() && values->items[6].as_bool(),
         "descending comparison chain should pass");
  expect(values->items[7].is_bool() && !values->items[7].as_bool(),
         "ascending comparison chain should fail");
  expect(values->items[8].is_bool() && !values->items[8].as_bool(),
         "descending comparison chain should fail");
}

void test_execute_emitted_integer_send_fast_path_new_ops() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def ops(x, y):\n"
              "  [x % y, x // y, x == y, x != y, x <=> y, x & y, "
              "x | y, x ^ y, x << y, x >> 1, x ** y]\n");
  expect(emit_result.module.methods.size() == 1, "expected ops method");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.methods[0].entry_code_id,
      {amber::runtime::Value::integer(10), amber::runtime::Value::integer(3)});
  expect(exec.ok(), "integer send fast path execution failed");
  expect(exec.value.is_list(), "integer send fast path should return list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 11,
         "integer send fast path list size");
  expect(values->items[0].is_integer() && values->items[0].as_integer() == 1,
         "integer SEND modulo should execute");
  expect(values->items[1].is_integer() && values->items[1].as_integer() == 3,
         "integer SEND floor division should execute");
  expect(values->items[2].is_bool() && !values->items[2].as_bool(),
         "integer SEND equality should execute");
  expect(values->items[3].is_bool() && values->items[3].as_bool(),
         "integer SEND inequality should execute");
  expect(values->items[4].is_integer() && values->items[4].as_integer() == 1,
         "integer SEND spaceship should execute");
  expect(values->items[5].is_integer() && values->items[5].as_integer() == 2,
         "integer SEND bit and should execute");
  expect(values->items[6].is_integer() && values->items[6].as_integer() == 11,
         "integer SEND bit or should execute");
  expect(values->items[7].is_integer() && values->items[7].as_integer() == 9,
         "integer SEND xor should execute");
  expect(values->items[8].is_integer() && values->items[8].as_integer() == 80,
         "integer SEND left shift should execute");
  expect(values->items[9].is_integer() && values->items[9].as_integer() == 5,
         "integer SEND right shift should execute");
  expect(values->items[10].is_integer() &&
             values->items[10].as_integer() == 1000,
         "integer SEND power should execute");
}

void test_runtime_integer_sidecar_result_returns_materialized_value() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  const std::uint32_t ten_id = append_integer_const(&module, 10);
  const std::uint32_t thirty_two_id = append_integer_const(&module, 32);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Module;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {ten_id, false}}});
  code.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {thirty_two_id, false}}});
  code.instructions.push_back(
      {Opcode::IAdd, {{2, false}, {0, false}, {1, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "integer sidecar return should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "integer sidecar result is materialized on return");
}

void test_runtime_integer_sidecar_materializes_completed_local() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.strings = {"total"};
  const std::uint32_t one_id = append_integer_const(&module, 1);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Module;
  code.reg_count = 1;
  code.local_layout.push_back({0, 0, 0, 0});
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {one_id, false}}});
  code.instructions.push_back(
      {Opcode::IAddK, {{0, false}, {0, false}, {one_id, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  const amber::runtime::ExecutionResult exec = world.execute(code.code_id);
  expect(exec.ok(), "integer sidecar completed local should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "integer sidecar completed local returns updated value");

  const amber::runtime::ExecutionLocal *total_local =
      execution_local_by_name(exec, "total");
  expect(total_local != nullptr, "integer sidecar local appears in locals");
  expect(total_local->initialized, "integer sidecar local is initialized");
  expect(total_local->value.is_integer() &&
             total_local->value.as_integer() == 2,
         "integer sidecar completed local is materialized");
}

void test_execute_emitted_compare_method() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def choose(x):\n"
                                                          "  if x > 0:\n"
                                                          "    x\n"
                                                          "  else:\n"
                                                          "    0\n");
  expect(emit_result.module.methods.size() == 1, "expected choose method");
  const std::uint32_t code_id = emit_result.module.methods[0].entry_code_id;

  const amber::runtime::ExecutionResult positive = amber::runtime::execute_code(
      emit_result.module, code_id, {amber::runtime::Value::integer(3)});
  expect(positive.ok(), "choose(3) execution failed");
  expect(positive.value.is_integer() && positive.value.as_integer() == 3,
         "choose(3) should return input");

  const amber::runtime::ExecutionResult negative = amber::runtime::execute_code(
      emit_result.module, code_id, {amber::runtime::Value::integer(-2)});
  expect(negative.ok(), "choose(-2) execution failed");
  expect(negative.value.is_integer() && negative.value.as_integer() == 0,
         "choose(-2) should return zero");
}

void test_execute_emitted_default_method() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x, y = x + 1):\n"
              "    y\n"
              "\n"
              "def probe():\n"
              "  Config.build(7)\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "defaulted probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "defaulted method execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 8,
         "default thunk should materialize y = x + 1");
}

void test_execute_emitted_keyword_method() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x, α:, β: 2):\n"
              "    x + α + β\n"
              "\n"
              "def probe():\n"
              "  Config.build(4, α: 5)\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "keyword probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "keyword method execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 11,
         "keyword shaping should bind α and materialize β default");
}

void test_execute_emitted_block_send() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Config:\n"
              "  class_method def build(x):\n"
              "    x\n"
              "\n"
              "def probe():\n"
              "  Config.build(4): 9\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "probe");
  expect(method != nullptr, "block send probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id);
  expect(exec.ok(), "block send execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 4,
         "user-defined SEND should accept forwarded block");
}

void test_execute_emitted_properties() {
  amber::bytecode::EmitResult emit_result =
      emit_ok("counter = 0\n"
              "prop next_value:\n"
              "  counter = counter + 1\n"
              "  counter\n"
              "next_value + next_value\n");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "top-level property execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 3,
         "top-level property should evaluate getter on each access");

  emit_result = emit_ok("class Point:\n"
                        "  def init(@x, @y): pass\n"
                        "  prop sum: @x + @y\n"
                        "Point(2, 5).sum\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "instance property execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "instance property should return getter result");

  emit_result = emit_ok("class Build:\n"
                        "  class_prop version: 203\n"
                        "Build.version\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "class property execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 203,
         "class property should return getter result");

  emit_result = emit_ok("class Box:\n"
                        "  def init(initial): @value = initial\n"
                        "  prop value:\n"
                        "    get: @value\n"
                        "    set(v):\n"
                        "      @value = v\n"
                        "      999\n"
                        "box = Box(1)\n"
                        "assigned = (box.value = 10)\n"
                        "box.value + assigned\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "instance property setter execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 20,
         "property assignment should return rhs and update property");

  emit_result = emit_ok("class Build:\n"
                        "  class_prop version:\n"
                        "    get: @@version\n"
                        "    set(v): @@version = v\n"
                        "Build.version = 204\n"
                        "Build.version\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "class property setter execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 204,
         "class property setter should update class-side value");

  emit_result =
      emit_ok("class User:\n"
              "  def init(@email): pass\n"
              "  attr email\n"
              "User(\"ada@example.test\").email == \"ada@example.test\"\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "getter-only attr execution failed");
  expect(exec.value.is_bool() && exec.value.as_bool(),
         "getter-only attr should read default storage");

  emit_result = emit_ok("class Box:\n"
                        "  def init(initial): @raw_value = initial\n"
                        "  attr var value from @raw_value\n"
                        "box = Box(4)\n"
                        "assigned = (box.value = 9)\n"
                        "box.value + assigned\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "read-write attr execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 18,
         "read-write attr should use explicit storage and return rhs");
}

void test_bare_nullary_member_implicit_call() {
  // RFC bare-nullary: `obj.member` performs an implicit zero-argument send
  // when `member` resolves to a syntactically nullary method.
  amber::bytecode::EmitResult emit_result = emit_ok("class Box:\n"
                                                    "  def value(): 1\n"
                                                    "Box().value\n");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "bare nullary member access execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 1,
         "bare access should implicitly call syntactically nullary def");

  emit_result = emit_ok("class Box:\n"
                        "  def value(): 1\n"
                        "Box().value() + Box().value\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "explicit and bare nullary call execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "explicit `value()` and bare `value` should both invoke nullary def");

  emit_result = emit_ok("class Build:\n"
                        "  class_method def version(): 20\n"
                        "Build.version\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "bare class-side nullary access execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 20,
         "bare class-side access should implicitly call nullary class method");
}

void test_bare_non_nullary_member_rejected() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Box:\n"
              "  def format(mode = 1): mode\n"
              "Box().format\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "ArgumentError",
         "bare access to non-nullary method should raise ArgumentError");
}

void test_property_called_as_method_rejected() {
  const amber::bytecode::EmitResult emit_result = emit_ok("class Box:\n"
                                                          "  prop value: 7\n"
                                                          "Box().value()\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "TypeError",
         "explicit call punctuation on a property should raise TypeError");
}

void test_dot_call_invokes_member_result() {
  // `obj.member.()` reads/implicitly-sends the member, then calls the
  // resulting callable value through the generic callable protocol.
  amber::bytecode::EmitResult emit_result =
      emit_ok("class Adder:\n"
              "  def call(x): x + 1\n"
              "class Factory:\n"
              "  prop provider: Adder()\n"
              "Factory().provider.(41)\n");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "dot-call on property value execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "`.()` should call the callable returned by a property");

  emit_result = emit_ok("class Answer:\n"
                        "  def call(): 43\n"
                        "class Factory:\n"
                        "  def provider(): Answer()\n"
                        "Factory().provider.()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(exec.ok(), "dot-call on nullary def result execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 43,
         "`.()` should call the callable returned by implicit nullary send");

  emit_result = emit_ok("x = 10\n"
                        "x.()\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "TypeError",
         "`.()` on a non-callable value should raise TypeError");
}

void test_property_access_errors() {
  amber::bytecode::EmitResult emit_result =
      emit_ok("class User:\n"
              "  prop password:\n"
              "    set(value): @password_hash = value\n"
              "user = User()\n"
              "user.password\n");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "WriteOnlyPropertyError",
         "read from write-only property should raise WriteOnlyPropertyError");

  emit_result = emit_ok("class Box:\n"
                        "  prop id: 5\n"
                        "box = Box()\n"
                        "box.id = 10\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "ReadOnlyPropertyError",
         "assignment to read-only property should raise "
         "ReadOnlyPropertyError");
}

void test_property_arm_is_non_suspendable() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Clock:\n"
              "  prop lazy_time:\n"
              "    task.sleep 0.01\n"
              "    42\n"
              "Clock().lazy_time\n");
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "EffectViolationError",
         "suspension inside property getter should raise "
         "EffectViolationError");
}

void test_kwargs_spread_requires_property() {
  amber::bytecode::EmitResult emit_result =
      emit_ok("class Sink:\n"
              "  class_method def show(mode: 0): mode\n"
              "class Options:\n"
              "  prop kwargs: {mode: 5}\n"
              "Sink.show(**Options())\n");
  amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, emit_result.module.init.entry_code_id);
  expect(exec.ok(), "kwargs property spread execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "object with readable `kwargs` property should spread keywords");

  emit_result = emit_ok("class Sink:\n"
                        "  class_method def show(mode: 0): mode\n"
                        "class Options:\n"
                        "  def kwargs(): {mode: 5}\n"
                        "Sink.show(**Options())\n");
  exec = amber::runtime::execute_code(emit_result.module,
                                      emit_result.module.init.entry_code_id);
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "TypeError" &&
             exec.fault->message.find("declare `prop kwargs`") !=
                 std::string::npos,
         "nullary def kwargs() must not participate in keyword spread and "
         "should hint at `prop kwargs`");
}

void test_manual_dynamic_send() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"+"};

  Constant plus;
  plus.kind = ConstantKind::SymbolRef;
  plus.ref_id = 0;
  module.const_pool.push_back(plus);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 4;
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  code.instructions.push_back({Opcode::LoadK, {{2, false}, {1, false}}});
  code.instructions.push_back({Opcode::SendDyn,
                               {{3, false},
                                {0, false},
                                {1, false},
                                {1, false},
                                {2, false},
                                {0, false},
                                {-1, true},
                                {0, false}}});
  code.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(8)});
  expect(exec.ok(), "dynamic send execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 10,
         "dynamic send should dispatch by symbol selector");
}

const amber::bytecode::BcMethod *
method_by_name(const amber::bytecode::BcModule &module,
               const std::string &name) {
  for (const amber::bytecode::BcMethod &method : module.methods) {
    if (method.selector_sym_id < module.symbols.size() &&
        module.symbols[method.selector_sym_id] == name) {
      return &method;
    }
  }
  return nullptr;
}

std::uint32_t symbol_id_or_die(const amber::bytecode::BcModule &module,
                               const std::string &name) {
  for (std::uint32_t i = 0; i < module.symbols.size(); ++i) {
    if (module.symbols[i] == name) {
      return i;
    }
  }
  std::cerr << "vm test failed: missing symbol " << name << "\n";
  std::exit(1);
}

std::uint32_t ensure_symbol_id(amber::bytecode::BcModule *module,
                               const std::string &name) {
  for (std::uint32_t i = 0; i < module->symbols.size(); ++i) {
    if (module->symbols[i] == name) {
      return i;
    }
  }
  module->symbols.push_back(name);
  return static_cast<std::uint32_t>(module->symbols.size() - 1U);
}

std::uint32_t append_string(amber::bytecode::BcModule *module,
                            const std::string &value) {
  for (std::uint32_t i = 0; i < module->strings.size(); ++i) {
    if (module->strings[i] == value) {
      return i;
    }
  }
  module->strings.push_back(value);
  return static_cast<std::uint32_t>(module->strings.size() - 1U);
}

std::uint32_t append_path_const(amber::bytecode::BcModule *module,
                                std::initializer_list<std::uint32_t> items) {
  amber::bytecode::Constant path;
  path.kind = amber::bytecode::ConstantKind::Path;
  path.items = items;
  module->const_pool.push_back(path);
  return static_cast<std::uint32_t>(module->const_pool.size() - 1U);
}

std::uint32_t append_integer_const(amber::bytecode::BcModule *module,
                                   std::int64_t value) {
  amber::bytecode::Constant constant;
  constant.kind = amber::bytecode::ConstantKind::Integer;
  constant.int_value = value;
  module->const_pool.push_back(constant);
  return static_cast<std::uint32_t>(module->const_pool.size() - 1U);
}

std::uint32_t append_float_const(amber::bytecode::BcModule *module,
                                 double value) {
  amber::bytecode::Constant constant;
  constant.kind = amber::bytecode::ConstantKind::Float;
  constant.float_value = value;
  module->const_pool.push_back(constant);
  return static_cast<std::uint32_t>(module->const_pool.size() - 1U);
}

std::string
string_value_text_or_die(const amber::runtime::Value &value,
                         const amber::bytecode::BcModule &module,
                         const amber::runtime::ExecutionResult &result) {
  expect(value.is_string(), "expected Str value");
  const std::uint32_t string_id = value.as_string().string_id;
  const std::vector<std::string> &strings =
      result.runtime_strings.empty() ? module.strings : result.runtime_strings;
  expect(string_id < strings.size(), "string id should be in runtime table");
  return strings[string_id];
}

amber::bytecode::Instruction
send_instr(std::uint32_t dst, std::uint32_t recv, std::uint32_t selector,
           const std::vector<std::uint32_t> &arg_regs, std::int64_t block_reg,
           std::uint32_t site_id);

amber::pkg::PackageArtifact
make_reload_artifact(const amber::bytecode::BcModule &module,
                     const std::string &version = "0.1.0") {
  amber::pkg::PackageArtifact artifact;
  artifact.manifest.name = "reload.pkg";
  artifact.manifest.version = version;
  artifact.manifest.root_module = "reload.core";
  artifact.manifest.modules.push_back({"reload.core", "src/core.am"});

  amber::pkg::PackageModuleBlob blob;
  blob.name = "reload.core";
  blob.path = "src/core.am";
  blob.bytes = amber::bytecode::serialize_module(module);
  artifact.modules.push_back(std::move(blob));
  return artifact;
}

amber::bytecode::BcModule make_reload_module(std::int64_t value,
                                             bool export_class = true,
                                             bool method_has_param = false) {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  const std::uint32_t box_id = ensure_symbol_id(&module, "Box");
  const std::uint32_t value_id = ensure_symbol_id(&module, "value");
  const std::uint32_t x_symbol_id = ensure_symbol_id(&module, "x");
  const std::uint32_t class_kind_id = append_string(&module, "class");
  const std::uint32_t x_string_id = append_string(&module, "x");
  const std::uint32_t param_role_id = append_string(&module, "param");
  const std::uint32_t local_kind_id = append_string(&module, "local");
  const std::uint32_t empty_signature_id = append_path_const(&module, {});
  const std::uint32_t integer_id = append_integer_const(&module, value);

  BcClass box;
  box.class_name_sym_id = box_id;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod method;
  method.selector_sym_id = value_id;
  method.owner_dispatch_ref = 0;
  method.signature_blob_id = empty_signature_id;
  method.entry_code_id = 2;
  method.flags = 1;
  if (method_has_param) {
    method.params.push_back({x_symbol_id, x_string_id, 0});
  }
  module.methods.push_back(method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.local_layout.push_back({0, x_string_id, param_role_id, local_kind_id});
  caller.instructions.push_back(send_instr(1, 0, value_id, {}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{1, false}}});
  caller.call_site_table.push_back({0, 0, value_id, 0});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {integer_id, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  if (export_class) {
    module.exports.push_back({box_id, class_kind_id, 0, 1, false, 0});
  }
  return module;
}

amber::runtime::Value make_closure_value(std::uint32_t code_id) {
  auto closure = amber::runtime::make_intrusive<amber::runtime::ClosureValue>();
  closure->header.kind = amber::runtime::HeapObjectKind::Closure;
  closure->code_id = code_id;
  return amber::runtime::Value::closure(std::move(closure));
}

amber::bytecode::Instruction
send_instr(std::uint32_t dst, std::uint32_t recv, std::uint32_t selector,
           const std::vector<std::uint32_t> &arg_regs = {},
           std::int64_t block_reg = -1, std::uint32_t site_id = 0) {
  amber::bytecode::Instruction insn;
  insn.opcode = amber::bytecode::Opcode::Send;
  insn.operands.push_back({dst, false});
  insn.operands.push_back({recv, false});
  insn.operands.push_back({selector, false});
  insn.operands.push_back({static_cast<std::int64_t>(arg_regs.size()), false});
  for (std::uint32_t reg : arg_regs) {
    insn.operands.push_back({reg, false});
  }
  insn.operands.push_back({0, false});
  insn.operands.push_back({block_reg, block_reg < 0});
  insn.operands.push_back({site_id, false});
  return insn;
}

amber::bytecode::Instruction send_kw_instr(
    std::uint32_t dst, std::uint32_t recv, std::uint32_t selector,
    const std::vector<std::uint32_t> &arg_regs,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &kw_regs,
    std::int64_t block_reg = -1, std::uint32_t site_id = 0) {
  amber::bytecode::Instruction insn;
  insn.opcode = amber::bytecode::Opcode::Send;
  insn.operands.push_back({dst, false});
  insn.operands.push_back({recv, false});
  insn.operands.push_back({selector, false});
  insn.operands.push_back({static_cast<std::int64_t>(arg_regs.size()), false});
  for (std::uint32_t reg : arg_regs) {
    insn.operands.push_back({reg, false});
  }
  insn.operands.push_back({static_cast<std::int64_t>(kw_regs.size()), false});
  for (const auto &[symbol_id, reg] : kw_regs) {
    insn.operands.push_back({symbol_id, false});
    insn.operands.push_back({reg, false});
  }
  insn.operands.push_back({block_reg, block_reg < 0});
  insn.operands.push_back({site_id, false});
  return insn;
}

void test_manual_integer_opcode_float_fallback() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  const std::uint32_t plus_id = ensure_symbol_id(&module, "+");
  (void)plus_id;
  const std::uint32_t lhs_id = append_float_const(&module, 1.5);
  const std::uint32_t rhs_id = append_integer_const(&module, 2);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {lhs_id, false}}});
  code.instructions.push_back(
      {Opcode::IAddK, {{1, false}, {0, false}, {rhs_id, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "IADDK float fallback execution failed");
  expect(exec.value.is_float() && exec.value.as_float() == 3.5,
         "IADDK should fallback to Float#+ for non-integer lhs");
}

amber::runtime::Value make_symbol_map(
    const amber::bytecode::BcModule &module,
    std::initializer_list<std::pair<const char *, amber::runtime::Value>>
        entries) {
  std::vector<amber::runtime::MapEntry> map_entries;
  map_entries.reserve(entries.size());
  for (const auto &entry : entries) {
    map_entries.push_back(
        {symbol_id_or_die(module, entry.first), entry.second});
  }
  return amber::runtime::make_symbol_map_value(std::move(map_entries));
}

const amber::runtime::RuntimeArenaStats *
arena_stats_for(const amber::runtime::RuntimeHeapStats &stats,
                std::uint64_t worker_id) {
  for (const amber::runtime::RuntimeArenaStats &arena : stats.arenas) {
    if (arena.worker_id == worker_id) {
      return &arena;
    }
  }
  return nullptr;
}

void test_runtime_duplicate_keyword_values_are_read_before_duplicate_check() {
  using namespace amber::bytecode;

  BcModule module;
  const std::uint32_t box_id = ensure_symbol_id(&module, "Box");
  const std::uint32_t route_id = ensure_symbol_id(&module, "route");
  const std::uint32_t alpha_id = ensure_symbol_id(&module, "α");
  const std::uint32_t alpha_string_id = append_string(&module, "α");
  append_path_const(&module, {});
  const std::uint32_t five_id = append_integer_const(&module, 5);

  BcClass box;
  box.class_name_sym_id = box_id;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod route;
  route.selector_sym_id = route_id;
  route.owner_dispatch_ref = 0;
  route.entry_code_id = 2;
  route.flags = 1;
  route.params.push_back({alpha_id, alpha_string_id, kMethodParamFlagKeyword});
  module.methods.push_back(route);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 3;
  caller.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {five_id, false}}});
  caller.instructions.push_back(
      send_kw_instr(2, 0, route_id, {}, {{alpha_id, 1}, {alpha_id, 2}}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult exec =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(!exec.ok(), "duplicate keyword with bad value should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "NameError",
         "keyword values should be read before duplicate keyword detection");
}

void test_runtime_keyword_shape_cache_canonicalizes_keyword_order() {
  using namespace amber::bytecode;

  BcModule module;
  const std::uint32_t box_id = ensure_symbol_id(&module, "Box");
  const std::uint32_t route_id = ensure_symbol_id(&module, "route");
  const std::uint32_t alpha_id = ensure_symbol_id(&module, "α");
  const std::uint32_t beta_id = ensure_symbol_id(&module, "β");
  const std::uint32_t alpha_string_id = append_string(&module, "α");
  const std::uint32_t beta_string_id = append_string(&module, "β");
  append_path_const(&module, {});

  BcClass box;
  box.class_name_sym_id = box_id;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod route;
  route.selector_sym_id = route_id;
  route.owner_dispatch_ref = 0;
  route.entry_code_id = 2;
  route.flags = 1;
  route.params.push_back({alpha_id, alpha_string_id, kMethodParamFlagKeyword});
  route.params.push_back({beta_id, beta_string_id, kMethodParamFlagKeyword});
  module.methods.push_back(route);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 7;
  caller.instructions.push_back(
      send_kw_instr(5, 0, route_id, {}, {{alpha_id, 1}, {beta_id, 2}}, -1, 0));
  caller.instructions.push_back(
      send_kw_instr(6, 0, route_id, {}, {{beta_id, 4}, {alpha_id, 3}}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{6, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 2;
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult exec = world.execute(
      1,
      {amber::runtime::Value::instance(instance),
       amber::runtime::Value::integer(3), amber::runtime::Value::integer(4),
       amber::runtime::Value::integer(8), amber::runtime::Value::integer(9)});
  expect(exec.ok(), "keyword shape cache order probe failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 8,
         "reversed keyword order should still bind by name");
  const amber::runtime::RuntimeDispatchCacheStats stats =
      world.dispatch_cache_stats();
  expect(stats.call_cache_entries == 1, "keyword cache should keep one entry");
  expect(stats.call_cache_misses == 1 && stats.call_cache_updates == 1 &&
             stats.call_cache_hits == 1,
         "canonical keyword shape should hit for reversed keyword order");
}

void test_runtime_call_cache_distinguishes_block_presence() {
  using namespace amber::bytecode;

  BcModule module;
  const std::uint32_t box_id = ensure_symbol_id(&module, "Box");
  const std::uint32_t value_id = ensure_symbol_id(&module, "value");
  append_path_const(&module, {});
  const std::uint32_t one_id = append_integer_const(&module, 1);

  BcClass box;
  box.class_name_sym_id = box_id;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod value;
  value.selector_sym_id = value_id;
  value.owner_dispatch_ref = 0;
  value.entry_code_id = 2;
  value.flags = 1;
  module.methods.push_back(value);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 4;
  caller.instructions.push_back(send_instr(2, 0, value_id, {}, -1, 0));
  caller.instructions.push_back(send_instr(3, 0, value_id, {}, 1, 0));
  caller.instructions.push_back({Opcode::Return, {{3, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {one_id, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode block;
  block.code_id = 3;
  block.kind = CodeKind::Block;
  block.reg_count = 1;
  block.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body, block};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult exec = world.execute(
      1, {amber::runtime::Value::instance(instance), make_closure_value(3)});
  expect(exec.ok(), "block presence cache guard probe failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 1,
         "block-presence probe should return method value");
  const amber::runtime::RuntimeDispatchCacheStats stats =
      world.dispatch_cache_stats();
  expect(stats.call_cache_entries == 1, "block guard should reuse site entry");
  expect(stats.call_cache_hits == 0 && stats.call_cache_misses == 2 &&
             stats.call_cache_updates == 2,
         "block presence should force a call-cache miss for same site");
}

void test_runtime_keyword_call_cache_invalidates_on_world_epoch() {
  using namespace amber::bytecode;

  BcModule module;
  const std::uint32_t box_id = ensure_symbol_id(&module, "Box");
  const std::uint32_t route_id = ensure_symbol_id(&module, "route");
  const std::uint32_t alpha_id = ensure_symbol_id(&module, "α");
  const std::uint32_t alpha_string_id = append_string(&module, "α");
  append_path_const(&module, {});
  const std::uint32_t one_id = append_integer_const(&module, 1);
  const std::uint32_t two_id = append_integer_const(&module, 2);

  BcClass box;
  box.class_name_sym_id = box_id;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod original;
  original.selector_sym_id = route_id;
  original.owner_dispatch_ref = 0;
  original.entry_code_id = 2;
  original.flags = 1;
  original.params.push_back(
      {alpha_id, alpha_string_id, kMethodParamFlagKeyword});
  module.methods.push_back(original);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 3;
  caller.instructions.push_back(
      send_kw_instr(2, 0, route_id, {}, {{alpha_id, 1}}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode body_one;
  body_one.code_id = 2;
  body_one.kind = CodeKind::Method;
  body_one.reg_count = 1;
  body_one.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {one_id, false}}});
  body_one.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_two;
  body_two.code_id = 3;
  body_two.kind = CodeKind::Method;
  body_two.reg_count = 1;
  body_two.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {two_id, false}}});
  body_two.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_one, body_two};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance),
                        amber::runtime::Value::integer(9)});
  expect(before.ok(), "keyword cache preflight send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "initial keyword method should return original value");
  amber::runtime::RuntimeDispatchCacheStats stats =
      world.dispatch_cache_stats();
  expect(stats.call_cache_misses == 1 && stats.call_cache_updates == 1 &&
             stats.call_cache_hits == 0,
         "initial keyword call should populate cache after one miss");

  const std::uint64_t epoch_before = world.world_epoch();
  BcMethod replacement = original;
  replacement.entry_code_id = 3;
  const amber::runtime::ExecutionResult defined =
      world.define_instance_method(0, replacement);
  expect(defined.ok(), "keyword cache method replacement failed");
  expect(world.world_epoch() == epoch_before + 1,
         "keyword method replacement should bump world epoch");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance),
                        amber::runtime::Value::integer(9)});
  expect(after.ok(), "keyword cache post-mutation send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "keyword call cache should invalidate after world mutation");
  stats = world.dispatch_cache_stats();
  expect(stats.call_cache_misses == 2 && stats.call_cache_updates == 2 &&
             stats.call_cache_hits == 0,
         "stale keyword cache entry should miss and refresh after epoch bump");
}

void test_execute_emitted_class_method_send() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  class_method def find(id):\n"
              "    id\n"
              "\n"
              "def probe():\n"
              "  Particle.find(4)\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "probe method exists");
  amber::runtime::RuntimeWorld table_world(emit_result.module);
  expect(table_world.method_table_size(
             0, amber::runtime::MethodTableSide::Class) == 1,
         "class-side method table should include emitted class method");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "class-side send execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 4,
         "class-side send should return class method result");
}

void test_execute_emitted_constructor_call() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def init(x):\n"
              "    @mass = x\n"
              "  def mass():\n"
              "    @mass\n"
              "\n"
              "def probe():\n"
              "  Particle(4).mass()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "constructor probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "constructor call execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 4,
         "constructor call should return initialized ivar");
}

void test_execute_emitted_constructor_auto_assign() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def init(@масса):\n"
              "    pass\n"
              "  def масса():\n"
              "    @масса\n"
              "\n"
              "def probe():\n"
              "  Particle(7).масса()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "auto-assign probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "constructor auto-assign execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "constructor auto-assign should materialize ivar");
}

void test_execute_emitted_constructor_default() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def init(x, y = x + 1):\n"
              "    @mass = y\n"
              "  def mass():\n"
              "    @mass\n"
              "\n"
              "def probe():\n"
              "  Particle(4).mass()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "constructor default probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "constructor default execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "constructor default should materialize trailing default param");
}

void test_execute_emitted_cvar_store_and_load() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Settings:\n"
              "  class_method def set(x):\n"
              "    @@ρ = x\n"
              "  class_method def get():\n"
              "    @@ρ\n"
              "\n"
              "def probe():\n"
              "  Settings.set(13)\n"
              "  Settings.get()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "cvar probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "cvar store/load execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 13,
         "class variable should round-trip through emitted methods");
}

void test_execute_emitted_constructor_cvar_auto_assign() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Settings:\n"
              "  def init(@@ρ):\n"
              "    pass\n"
              "  class_method def ρ():\n"
              "    @@ρ\n"
              "\n"
              "def probe():\n"
              "  Settings(17)\n"
              "  Settings.ρ()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "cvar auto-assign probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "cvar auto-assign execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 17,
         "constructor auto-assign should materialize class variable");
}

void test_execute_emitted_superclass_dispatch() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Base:\n"
              "  def mass():\n"
              "    11\n"
              "\n"
              "class Particle < Base:\n"
              "  def own():\n"
              "    0\n"
              "\n"
              "def probe():\n"
              "  Particle().mass()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "superclass probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "superclass dispatch execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 11,
         "instance SEND should fall through superclass chain");
}

void test_execute_emitted_include_linearization() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("mixin Older:\n"
              "  def value():\n"
              "    1\n"
              "\n"
              "mixin Newer:\n"
              "  def value():\n"
              "    2\n"
              "\n"
              "class Box:\n"
              "  include Older, Newer\n"
              "\n"
              "def probe():\n"
              "  Box().value()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "include probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "include dispatch execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "later include should win in instance-side lookup");
}

void test_execute_emitted_extend_linearization() {
  const amber::bytecode::EmitResult emit_result = emit_ok("mixin Tagged:\n"
                                                          "  def label():\n"
                                                          "    23\n"
                                                          "\n"
                                                          "class Box:\n"
                                                          "  extend Tagged\n"
                                                          "\n"
                                                          "def probe():\n"
                                                          "  Box.label()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "extend probe method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "extend dispatch execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 23,
         "class-side lookup should see extended mixin methods");
}

void test_execute_emitted_method_missing_instance() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Proxy:\n"
              "  def method_missing(name, α:):\n"
              "    α\n"
              "\n"
              "def probe():\n"
              "  Proxy().unknown(α: 5)\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "instance method_missing probe exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "instance method_missing execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 5,
         "method_missing should receive forwarded keyword arguments");
}

void test_execute_emitted_method_missing_class_side() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Proxy:\n"
              "  class_method def method_missing(name):\n"
              "    29\n"
              "\n"
              "def probe():\n"
              "  Proxy.unknown()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "class method_missing probe exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(exec.ok(), "class method_missing execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 29,
         "class-side miss should fall back to class method_missing");
}

void test_method_missing_does_not_recurse() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Proxy:\n"
              "  def own():\n"
              "    0\n"
              "\n"
              "def probe():\n"
              "  Proxy().method_missing()\n");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit_result.module, "probe");
  expect(probe != nullptr, "non-recursive method_missing probe exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, probe->entry_code_id);
  expect(!exec.ok(), "missing method_missing should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "NoMethodError",
         "method_missing selector should not recurse into itself");
}

void test_execute_emitted_case_literal() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def classify(x):\n"
                                                          "  case x:\n"
                                                          "    when 1:\n"
                                                          "      11\n"
                                                          "    else:\n"
                                                          "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "classify");
  expect(method != nullptr, "literal case method exists");

  const amber::runtime::ExecutionResult hit =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(1)});
  expect(hit.ok(), "literal case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 11,
         "literal case should take matching arm");

  const amber::runtime::ExecutionResult miss =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(2)});
  expect(miss.ok(), "literal case miss failed");
  expect(miss.value.is_integer() && miss.value.as_integer() == 0,
         "literal case should take else arm");
}

void test_execute_emitted_case_pin() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def same(x, y):\n"
                                                          "  case y:\n"
                                                          "    when ^x:\n"
                                                          "      1\n"
                                                          "    else:\n"
                                                          "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "same");
  expect(method != nullptr, "pin case method exists");

  const amber::runtime::ExecutionResult hit = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::Value::integer(7), amber::runtime::Value::integer(7)});
  expect(hit.ok(), "pin case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 1,
         "pin case should match equal value");

  const amber::runtime::ExecutionResult miss = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::Value::integer(7), amber::runtime::Value::integer(8)});
  expect(miss.ok(), "pin case miss failed");
  expect(miss.value.is_integer() && miss.value.as_integer() == 0,
         "pin case should fall through to else");
}

void test_execute_emitted_case_bind() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def mirror(x):\n"
                                                          "  case x:\n"
                                                          "    when y:\n"
                                                          "      y\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "mirror");
  expect(method != nullptr, "bind case method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(9)});
  expect(exec.ok(), "bind case execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 9,
         "bind case should materialize bound local");
}

void test_execute_emitted_case_bang_failure() {
  const amber::bytecode::EmitResult emit_result = emit_ok("def classify!(x):\n"
                                                          "  case! x:\n"
                                                          "    when 1:\n"
                                                          "      11\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "classify!");
  expect(method != nullptr, "case! method exists");

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::integer(2)});
  expect(!exec.ok(), "case! miss should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "MatchError",
         "case! miss should raise MatchError");
}

void test_execute_emitted_case_const_class() {
  const amber::bytecode::EmitResult emit_result = emit_ok("class Marker:\n"
                                                          "  def own():\n"
                                                          "    0\n"
                                                          "\n"
                                                          "def classify(x):\n"
                                                          "  case x:\n"
                                                          "    when Marker:\n"
                                                          "      1\n"
                                                          "    else:\n"
                                                          "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "classify");
  expect(method != nullptr, "const class case method exists");
  expect(!emit_result.module.classes.empty(), "marker class emitted");

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult hit =
      amber::runtime::execute_code(emit_result.module, method->entry_code_id,
                                   {amber::runtime::Value::instance(instance)});
  expect(hit.ok(), "const class case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 1,
         "const class pattern should match instance of class");
}

void test_execute_emitted_case_list_exact() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def second(values):\n"
              "  case values:\n"
              "    when [1, x]:\n"
              "      x\n"
              "    else:\n"
              "      0\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "second");
  expect(method != nullptr, "list exact case method exists");

  const amber::runtime::ExecutionResult hit = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::make_list_value({amber::runtime::Value::integer(1),
                                        amber::runtime::Value::integer(9)})});
  expect(hit.ok(), "list exact case hit failed");
  expect(hit.value.is_integer() && hit.value.as_integer() == 9,
         "list exact case should bind second element");

  const amber::runtime::ExecutionResult miss = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::make_list_value({amber::runtime::Value::integer(1),
                                        amber::runtime::Value::integer(9),
                                        amber::runtime::Value::integer(10)})});
  expect(miss.ok(), "list exact case miss failed");
  expect(miss.value.is_integer() && miss.value.as_integer() == 0,
         "list exact case should reject extra elements");
}

void test_execute_emitted_pattern_assign_list_rest() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("def unpack(values):\n"
              "  [head, *tail] = values\n"
              "  tail\n");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "unpack");
  expect(method != nullptr, "pattern assignment method exists");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {amber::runtime::make_list_value({amber::runtime::Value::integer(3),
                                        amber::runtime::Value::integer(4),
                                        amber::runtime::Value::integer(5)})});
  expect(exec.ok(), "pattern assignment execution failed");
  expect(exec.value.is_list(), "pattern assignment tail should be a list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> tail =
      exec.value.as_list();
  expect(tail != nullptr && tail->items.size() == 2,
         "pattern assignment tail should have two items");
  expect(tail->items[0].is_integer() && tail->items[0].as_integer() == 4 &&
             tail->items[1].is_integer() && tail->items[1].as_integer() == 5,
         "pattern assignment tail should contain remaining elements");
}

void test_execute_emitted_case_map_rest() {
  amber::bytecode::EmitResult emit_result = emit_ok("def capture(payload):\n"
                                                    "  case payload:\n"
                                                    "    when {a:, **rest}:\n"
                                                    "      rest\n"
                                                    "    else:\n"
                                                    "      null\n");
  ensure_symbol_id(&emit_result.module, "b");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "capture");
  expect(method != nullptr, "map-rest case method exists");

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {make_symbol_map(emit_result.module,
                       {{"a", amber::runtime::Value::integer(1)},
                        {"b", amber::runtime::Value::integer(7)}})});
  expect(exec.ok(), "map-rest case execution failed");
  expect(exec.value.is_map(), "map-rest case should return rest map");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> rest =
      exec.value.as_map();
  expect(rest != nullptr && rest->entries.size() == 1,
         "map-rest case should keep one extra key");
  expect(rest->entries[0].symbol_id ==
                 symbol_id_or_die(emit_result.module, "b") &&
             rest->entries[0].value.is_integer() &&
             rest->entries[0].value.as_integer() == 7,
         "map-rest case should capture remaining field");
}

void test_execute_emitted_case_map_strict_null() {
  amber::bytecode::EmitResult emit_result = emit_ok("def strict(payload):\n"
                                                    "  case payload:\n"
                                                    "    when {a:, **null}:\n"
                                                    "      1\n"
                                                    "    else:\n"
                                                    "      0\n");
  ensure_symbol_id(&emit_result.module, "b");
  const amber::bytecode::BcMethod *method =
      method_by_name(emit_result.module, "strict");
  expect(method != nullptr, "strict map case method exists");

  const amber::runtime::ExecutionResult exact = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {make_symbol_map(emit_result.module,
                       {{"a", amber::runtime::Value::integer(1)}})});
  expect(exact.ok(), "strict map exact execution failed");
  expect(exact.value.is_integer() && exact.value.as_integer() == 1,
         "strict map should accept exact key set");

  const amber::runtime::ExecutionResult extra = amber::runtime::execute_code(
      emit_result.module, method->entry_code_id,
      {make_symbol_map(emit_result.module,
                       {{"a", amber::runtime::Value::integer(1)},
                        {"b", amber::runtime::Value::integer(2)}})});
  expect(extra.ok(), "strict map extra-key execution failed");
  expect(extra.value.is_integer() && extra.value.as_integer() == 0,
         "strict map should reject extra keys");
}

void test_execute_emitted_clause_method_dispatch() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def mass(0): 1\n"
              "  def mass(n) if n > 0: n\n"
              "\n"
              "def zero():\n"
              "  Particle().mass(0)\n"
              "\n"
              "def positive():\n"
              "  Particle().mass(4)\n"
              "\n"
              "def negative():\n"
              "  Particle().mass(2 - 3)\n");
  const amber::bytecode::BcMethod *zero =
      method_by_name(emit_result.module, "zero");
  const amber::bytecode::BcMethod *positive =
      method_by_name(emit_result.module, "positive");
  const amber::bytecode::BcMethod *negative =
      method_by_name(emit_result.module, "negative");
  expect(zero != nullptr && positive != nullptr && negative != nullptr,
         "clause dispatch probes exist");

  const amber::runtime::ExecutionResult zero_result =
      amber::runtime::execute_code(emit_result.module, zero->entry_code_id);
  expect(zero_result.ok(), "clause dispatch zero execution failed");
  expect(zero_result.value.is_integer() && zero_result.value.as_integer() == 1,
         "first clause should match zero");

  const amber::runtime::ExecutionResult positive_result =
      amber::runtime::execute_code(emit_result.module, positive->entry_code_id);
  expect(positive_result.ok(), "clause dispatch positive execution failed");
  expect(positive_result.value.is_integer() &&
             positive_result.value.as_integer() == 4,
         "guarded clause should match positive argument");

  const amber::runtime::ExecutionResult negative_result =
      amber::runtime::execute_code(emit_result.module, negative->entry_code_id);
  expect(!negative_result.ok(), "clause dispatch miss should fail");
  expect(negative_result.fault.has_value() &&
             negative_result.fault->error_name == "MatchError",
         "clause dispatch miss should raise MatchError");
}

void test_manual_make_map() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"α", "β"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool = {one, two};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 4;
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {1, false}}});
  code.instructions.push_back({Opcode::MakeMap,
                               {{2, false},
                                {2, false},
                                {0, false},
                                {0, false},
                                {1, false},
                                {1, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "MAKE_MAP execution failed");
  expect(exec.value.is_map(), "MAKE_MAP should materialize map value");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> map =
      exec.value.as_map();
  expect(map != nullptr && map->entries.size() == 2,
         "MAKE_MAP should preserve two entries");
  expect(map->entries[0].symbol_id == 0 && map->entries[0].value.is_integer() &&
             map->entries[0].value.as_integer() == 1,
         "MAKE_MAP should preserve first entry");
  expect(map->entries[1].symbol_id == 1 && map->entries[1].value.is_integer() &&
             map->entries[1].value.as_integer() == 2,
         "MAKE_MAP should preserve second entry");
}

void expect_integer_list(const amber::runtime::Value &value,
                         const std::vector<std::int64_t> &expected,
                         const std::string &message) {
  expect(value.is_list(), message + " should be a list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      value.as_list();
  expect(list != nullptr && list->items.size() == expected.size(),
         message + " list size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect(list->items[i].is_integer() &&
               list->items[i].as_integer() == expected[i],
           message + " item " + std::to_string(i));
  }
}

amber::runtime::ExecutionResult
execute_emitted_init(const std::string &source) {
  const amber::bytecode::EmitResult emit_result = emit_ok(source);
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  expect(decoded.module.init.has_entry_code_id, "emitted module init exists");
  return amber::runtime::execute_code(decoded.module,
                                      decoded.module.init.entry_code_id);
}

const amber::runtime::ExecutionLocal *
execution_local_by_name(const amber::runtime::ExecutionResult &result,
                        const std::string &name) {
  for (const amber::runtime::ExecutionLocal &local : result.locals) {
    if (local.name == name) {
      return &local;
    }
  }
  return nullptr;
}

const amber::runtime::RuntimeDependency *
dependency_by_kind(const amber::runtime::RuntimeDependencySet &set,
                   amber::runtime::RuntimeDependencyKind kind) {
  for (const amber::runtime::RuntimeDependency &dependency : set.dependencies) {
    if (dependency.kind == kind) {
      return &dependency;
    }
  }
  return nullptr;
}

void test_execute_emitted_block_map_suffixes() {
  const amber::runtime::ExecutionResult implicit =
      execute_emitted_init("[1,2].map: _1 * 2\n");
  expect(implicit.ok(), "implicit placeholder map block should execute");
  expect_integer_list(implicit.value, {2, 4}, "implicit placeholder map block");

  const amber::runtime::ExecutionResult explicit_param =
      execute_emitted_init("[1,2].map |x|: x * 2\n");
  expect(explicit_param.ok(), "explicit param map block should execute");
  expect_integer_list(explicit_param.value, {2, 4}, "explicit param map block");

  const amber::runtime::ExecutionResult implicit_indented =
      execute_emitted_init("[1,2].map:\n"
                           "  doubled = _1 * 2\n"
                           "  doubled + 1\n");
  expect(implicit_indented.ok(),
         "implicit placeholder indented map block should execute");
  expect_integer_list(implicit_indented.value, {3, 5},
                      "implicit placeholder indented map block");

  const amber::runtime::ExecutionResult explicit_indented =
      execute_emitted_init("[1,2].map |x|:\n"
                           "  doubled = x * 2\n"
                           "  doubled + 1\n");
  expect(explicit_indented.ok(),
         "explicit param indented map block should execute");
  expect_integer_list(explicit_indented.value, {3, 5},
                      "explicit param indented map block");
}

void test_execute_emitted_v20_5_array_generation_and_optional_access() {
  const amber::runtime::ExecutionResult exec = execute_emitted_init(
      "generated = Array.of(4) |i|:\n"
      "  i + 1\n"
      "implicit = Array.of(3):\n"
      "  _1 * 2\n"
      "built = Array.build(3) |i|:\n"
      "  i + 10\n"
      "filled = Array.filled(4, false)\n"
      "numbers = [10, 20, 30]\n"
      "m = {a: 1, b: null}\n"
      "[generated[0], generated[3], implicit[2], built[1], filled[0], "
      "numbers[?0], numbers[?2], numbers[?3], numbers[?-1], "
      "numbers.has_index?(2), numbers.has_index?(3), m[?:a], m[?:b], "
      "m[?:c], m.contains?(:b), m.contains?(:c)]\n");
  expect(exec.ok(), "v20.5 array/optional access probe should execute");
  expect(exec.value.is_list(), "v20.5 probe should return list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> values =
      exec.value.as_list();
  expect(values != nullptr && values->items.size() == 16,
         "v20.5 probe result shape");
  const std::vector<std::int64_t> int_indices = {0, 1, 2, 3, 5, 6, 8, 11};
  const std::vector<std::int64_t> int_values = {1, 4, 4, 11, 10, 30, 30, 1};
  for (std::size_t i = 0; i < int_indices.size(); ++i) {
    const std::size_t index = static_cast<std::size_t>(int_indices[i]);
    expect(values->items[index].is_integer() &&
               values->items[index].as_integer() == int_values[i],
           "v20.5 integer result " + std::to_string(index));
  }
  expect(values->items[4].is_bool() && !values->items[4].as_bool(),
         "Array.filled repeats false value");
  expect(values->items[7].is_null(), "array optional out of bounds is null");
  expect(values->items[9].is_bool() && values->items[9].as_bool(),
         "Array#has_index? present");
  expect(values->items[10].is_bool() && !values->items[10].as_bool(),
         "Array#has_index? absent");
  expect(values->items[12].is_null(), "map optional stored null is null");
  expect(values->items[13].is_null(), "map optional missing key is null");
  expect(values->items[14].is_bool() && values->items[14].as_bool(),
         "Map#contains? sees stored null key");
  expect(values->items[15].is_bool() && !values->items[15].as_bool(),
         "Map#contains? rejects missing key");

  const amber::runtime::ExecutionResult fresh =
      execute_emitted_init("Array.of(2):\n"
                           "  []\n");
  expect(fresh.ok() && fresh.value.is_list(),
         "Array.of should create nested arrays");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> fresh_items =
      fresh.value.as_list();
  expect(fresh_items != nullptr && fresh_items->items.size() == 2 &&
             fresh_items->items[0].is_list() &&
             fresh_items->items[1].is_list() &&
             fresh_items->items[0].as_list() != fresh_items->items[1].as_list(),
         "Array.of evaluates block once per slot");

  const amber::runtime::ExecutionResult shared =
      execute_emitted_init("Array.filled(2, [])\n");
  expect(shared.ok() && shared.value.is_list(),
         "Array.filled should create outer array");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> shared_items =
      shared.value.as_list();
  expect(shared_items != nullptr && shared_items->items.size() == 2 &&
             shared_items->items[0].is_list() &&
             shared_items->items[1].is_list() &&
             shared_items->items[0].as_list() ==
                 shared_items->items[1].as_list(),
         "Array.filled repeats the same value reference");

  const amber::runtime::ExecutionResult missing_block =
      execute_emitted_init("Array.of(1)\n");
  expect(!missing_block.ok() && missing_block.fault.has_value() &&
             missing_block.fault->error_name == "ArgumentError",
         "Array.of without block reports ArgumentError");

  const amber::runtime::ExecutionResult negative =
      execute_emitted_init("Array.filled(-1, 0)\n");
  expect(!negative.ok() && negative.fault.has_value() &&
             negative.fault->error_name == "ArgumentError",
         "Array.filled negative length reports ArgumentError");

  const amber::runtime::ExecutionResult non_int =
      execute_emitted_init("Array.filled(\"2\", 0)\n");
  expect(!non_int.ok() && non_int.fault.has_value() &&
             non_int.fault->error_name == "TypeError",
         "Array.filled non-Int length reports TypeError");
}

void test_runtime_watch_local_storage_replacement() {
  const amber::bytecode::EmitResult emit_result = emit_ok("x = 1\n"
                                                          "Kernel.watch(x)\n"
                                                          "x = 1\n"
                                                          "x = 2\n"
                                                          "x\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  expect(decoded.module.init.has_entry_code_id, "watch local module init");

  amber::runtime::RuntimeWorld world(decoded.module);
  const std::uint64_t world_epoch_before = world.world_epoch();
  const amber::runtime::ExecutionResult exec =
      world.execute(decoded.module.init.entry_code_id);
  expect(exec.ok(), "watch local module should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "watched local returns current value");
  expect(world.world_epoch() == world_epoch_before,
         "watch local does not mutate dispatch world epoch");
  expect(world.watch_epoch() == 3, "watch local updates watch epoch");
  expect(exec.watch_epoch == 3,
         "watch local bumps watch epoch only for events");
  expect(exec.watch_events.size() == 3, "watch local records three events");
  expect(exec.watch_events[0].kind == "watch.binding",
         "watch local records binding event");
  expect(exec.watch_events[0].target_name == "x",
         "watch binding event names local");
  expect(exec.watch_events[1].kind == "watch.write",
         "watched local same-value assignment records write event");
  expect(exec.watch_events[1].old_revision == 0 &&
             exec.watch_events[1].new_revision == 1,
         "watched local same-value write advances revision");
  expect(exec.watch_events[1].old_value.is_integer() &&
             exec.watch_events[1].old_value.as_integer() == 1,
         "watched local same-value write records old value");
  expect(exec.watch_events[1].new_value.is_integer() &&
             exec.watch_events[1].new_value.as_integer() == 1,
         "watched local same-value write records new value");
  expect(exec.watch_events[2].kind == "watch.write",
         "watched local assignment records write event");
  expect(exec.watch_events[2].old_revision == 1 &&
             exec.watch_events[2].new_revision == 2,
         "watched local write advances revision");
  expect(exec.watch_events[2].old_value.is_integer() &&
             exec.watch_events[2].old_value.as_integer() == 1,
         "watched local write records old value");
  expect(exec.watch_events[2].new_value.is_integer() &&
             exec.watch_events[2].new_value.as_integer() == 2,
         "watched local write records new value");

  const amber::runtime::ExecutionLocal *x_local =
      execution_local_by_name(exec, "x");
  expect(x_local != nullptr, "watched local appears in execution locals");
  expect(x_local->initialized, "watched local remains initialized");
  expect(x_local->watched, "execution local exposes watched storage");
  expect(x_local->watch_cell_id != 0, "execution local exposes watch cell id");
  expect(x_local->watch_revision == 2,
         "execution local exposes watch revision");
  expect(x_local->value.is_integer() && x_local->value.as_integer() == 2,
         "execution local exposes unwrapped watched value");
}

void test_runtime_integer_specialized_op_preserves_watch_local_write() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.strings = {"x"};
  const std::uint32_t one_id = append_integer_const(&module, 1);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Module;
  code.reg_count = 2;
  code.local_layout.push_back({0, 0, 0, 0});
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {one_id, false}}});
  code.instructions.push_back(
      {Opcode::WatchLocal, {{1, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::IAddK, {{0, false}, {0, false}, {one_id, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  const amber::runtime::ExecutionResult exec = world.execute(code.code_id);
  expect(exec.ok(), "watched integer add should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "watched integer add returns updated value");
  expect(exec.watch_events.size() == 2,
         "watched integer add records binding and write events");
  expect(exec.watch_events[1].kind == "watch.write",
         "watched integer add records write event");
  expect(exec.watch_events[1].target_name == "x",
         "watched integer add writes watched local");
  expect(exec.watch_events[1].new_value.is_integer() &&
             exec.watch_events[1].new_value.as_integer() == 2,
         "watched integer add records new value");
}

void test_runtime_compare_branch_preserves_debug_local() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.strings = {"cmp"};
  const std::uint32_t one_id = append_integer_const(&module, 1);
  const std::uint32_t two_id = append_integer_const(&module, 2);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Module;
  code.reg_count = 3;
  code.local_layout.push_back({0, 0, 0, 0});
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {one_id, false}}});
  code.instructions.push_back({Opcode::LoadK, {{2, false}, {two_id, false}}});
  code.instructions.push_back(
      {Opcode::ILt, {{0, false}, {1, false}, {2, false}}});
  code.instructions.push_back({Opcode::JumpIfFalse, {{0, false}, {6, false}}});
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {two_id, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {one_id, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  const amber::runtime::ExecutionResult exec = world.execute(code.code_id);
  expect(exec.ok(), "compare branch with debug local should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "compare branch should take true path");

  const amber::runtime::ExecutionLocal *cmp_local =
      execution_local_by_name(exec, "cmp");
  expect(cmp_local != nullptr, "compare local appears in execution locals");
  expect(cmp_local->initialized, "compare local remains initialized");
  expect(cmp_local->value.is_bool() && cmp_local->value.as_bool(),
         "compare local preserves boolean value");
}

void test_runtime_watch_capture_uses_shared_storage_cell() {
  using namespace amber::bytecode;

  BcModule module;
  module.strings = {"x"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool = {one, two};

  BcCode outer;
  outer.code_id = 1;
  outer.kind = CodeKind::Method;
  outer.reg_count = 4;
  outer.local_layout.push_back({0, 0, 0, 0});
  outer.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  outer.instructions.push_back(
      {Opcode::MakeClosure,
       {{1, false}, {2, false}, {1, false}, {0, false}, {0, false}}});
  outer.instructions.push_back(
      {Opcode::WatchLocal, {{2, false}, {0, false}, {0, false}}});
  outer.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  outer.instructions.push_back({Opcode::Call,
                                {{3, false},
                                 {1, false},
                                 {0, false},
                                 {0, false},
                                 {-1, true},
                                 {0, false}}});
  outer.instructions.push_back({Opcode::Return, {{3, false}}});

  BcCode inner;
  inner.code_id = 2;
  inner.kind = CodeKind::Block;
  inner.reg_count = 1;
  inner.capture_layout.push_back({0, 0, 0, 0});
  inner.instructions.push_back({Opcode::LoadUpval, {{0, false}, {0, false}}});
  inner.instructions.push_back({Opcode::Return, {{0, false}}});

  module.code_objects = {outer, inner};

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "watch capture module should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "watched captured local is updated through shared storage");
  expect(exec.watch_epoch == 2, "watch capture updates watch epoch");
  expect(exec.watch_events.size() == 2, "watch capture records two events");
  expect(exec.watch_events[0].kind == "watch.binding",
         "watch capture records binding event");
  expect(exec.watch_events[1].kind == "watch.write",
         "watch capture records write event");
  expect(exec.watch_events[1].target_name == "x",
         "watch capture write event keeps binding name");
  expect(exec.watch_events[1].new_value.is_integer() &&
             exec.watch_events[1].new_value.as_integer() == 2,
         "watch capture write records updated value");
}

void test_runtime_watch_ivar_records_object_revision_events() {
  using namespace amber::bytecode;

  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def observe():\n"
              "    Kernel.watch(@mass)\n"
              "    @mass = 1\n"
              "    @mass = 1\n"
              "    @mass\n"
              "\n"
              "def probe():\n"
              "  Particle().observe()\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  const amber::bytecode::BcMethod *probe =
      method_by_name(decoded.module, "probe");
  expect(probe != nullptr, "watch ivar probe method exists");

  amber::runtime::RuntimeWorld world(decoded.module);
  const std::uint64_t world_epoch_before = world.world_epoch();
  const amber::runtime::ExecutionResult exec =
      world.execute(probe->entry_code_id);
  expect(exec.ok(), "watch ivar module should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 1,
         "watched ivar returns current value");
  expect(world.world_epoch() == world_epoch_before,
         "watch ivar does not mutate dispatch world epoch");
  expect(exec.watch_epoch == 3, "watch ivar updates watch epoch");
  expect(exec.watch_events.size() == 3, "watch ivar records three events");

  const amber::runtime::RuntimeWatchEvent &binding = exec.watch_events[0];
  expect(binding.kind == "watch.ivar", "watch ivar records binding event");
  expect(binding.target_name == "@mass" && binding.field_name == "mass",
         "watch ivar binding names field target");
  expect(binding.object_id != 0, "watch ivar binding exposes object id");
  expect(binding.old_revision == 0 && binding.new_revision == 0,
         "watch ivar binding starts field revision at zero");
  expect(binding.old_object_revision == 0 && binding.new_object_revision == 0,
         "watch ivar binding starts object revision at zero");

  const amber::runtime::RuntimeWatchEvent &first_write = exec.watch_events[1];
  expect(first_write.kind == "watch.ivar.write",
         "watched ivar assignment records write event");
  expect(first_write.object_id == binding.object_id,
         "watched ivar write keeps object id");
  expect(first_write.old_revision == 0 && first_write.new_revision == 1,
         "watched ivar first write advances field revision");
  expect(first_write.old_object_revision == 0 &&
             first_write.new_object_revision == 1,
         "watched ivar first write advances object revision");
  expect(first_write.old_value.is_null(),
         "watched ivar first write records null old value");
  expect(first_write.new_value.is_integer() &&
             first_write.new_value.as_integer() == 1,
         "watched ivar first write records new value");

  const amber::runtime::RuntimeWatchEvent &second_write = exec.watch_events[2];
  expect(second_write.kind == "watch.ivar.write",
         "watched ivar same-value assignment records write event");
  expect(second_write.old_revision == 1 && second_write.new_revision == 2,
         "watched ivar same-value write advances field revision");
  expect(second_write.old_object_revision == 1 &&
             second_write.new_object_revision == 2,
         "watched ivar same-value write advances object revision");
  expect(second_write.old_value.is_integer() &&
             second_write.old_value.as_integer() == 1,
         "watched ivar same-value write records old value");
  expect(second_write.new_value.is_integer() &&
             second_write.new_value.as_integer() == 1,
         "watched ivar same-value write records new value");

  BcModule failed_module;
  failed_module.symbols = {"mass"};
  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  failed_module.const_pool = {two};

  BcCode failed;
  failed.code_id = 1;
  failed.kind = CodeKind::Method;
  failed.reg_count = 4;
  failed.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  failed.instructions.push_back(
      {Opcode::WatchIvar,
       {{2, false}, {1, false}, {0, false}, {0, false}, {0, false}}});
  failed.instructions.push_back({Opcode::ObjDealloc, {{3, false}, {1, false}}});
  failed.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  failed.instructions.push_back(
      {Opcode::StoreIvar, {{1, false}, {0, false}, {0, false}, {1, false}}});
  failed.instructions.push_back({Opcode::Return, {{0, false}}});
  failed_module.code_objects.push_back(failed);

  auto failed_instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  failed_instance->class_index = 0;
  amber::runtime::RuntimeWorld failed_world(failed_module);
  const amber::runtime::ExecutionResult failed_exec = failed_world.execute(
      1, {}, amber::runtime::Value::instance(failed_instance));
  expect(!failed_exec.ok(), "failed watched ivar write should fault");
  expect(failed_exec.watch_events.size() == 1,
         "failed watched ivar write should not publish write event");
  expect(failed_exec.watch_events[0].kind == "watch.ivar",
         "failed watched ivar write keeps only binding event");
}

void test_runtime_dependency_capture_records_binding_reads() {
  const amber::bytecode::EmitResult emit_result = emit_ok("x = 1\n"
                                                          "Kernel.watch(x)\n"
                                                          "x\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  expect(decoded.module.init.has_entry_code_id,
         "dependency capture module init exists");

  amber::runtime::RuntimeWorld world(decoded.module);
  const std::uint64_t watch_epoch_before = world.watch_epoch();
  world.begin_dependency_capture(42);
  const amber::runtime::ExecutionResult exec =
      world.execute(decoded.module.init.entry_code_id);
  expect(exec.ok(), "dependency capture binding module should execute");

  const amber::runtime::RuntimeDependencySet active =
      world.dependency_capture_snapshot();
  expect(active.notebook_cell_id == 42,
         "active dependency capture keeps notebook cell id");
  expect(active.dependencies.size() == 1,
         "active dependency capture deduplicates binding reads");

  const amber::runtime::RuntimeDependencySet deps =
      world.end_dependency_capture();
  expect(deps.notebook_cell_id == 42,
         "ended dependency capture returns notebook cell id");
  expect(deps.dependencies.size() == 1,
         "ended dependency capture returns one binding dependency");
  const amber::runtime::RuntimeDependency *binding =
      dependency_by_kind(deps, amber::runtime::RuntimeDependencyKind::Binding);
  expect(binding != nullptr, "dependency capture exposes binding dependency");
  expect(binding->cell_id == exec.watch_events[0].cell_id,
         "binding dependency uses watch cell id");
  expect(binding->target_name == "x", "binding dependency keeps target name");
  expect(binding->revision == 0, "binding dependency records current revision");
  expect(binding->object_id == 0 && binding->field_name.empty(),
         "binding dependency does not set object fields");
  expect(world.watch_epoch() == watch_epoch_before + 1,
         "dependency capture itself does not publish watch events");
  expect(world.dependency_capture_snapshot().dependencies.empty(),
         "dependency capture snapshot is empty after end");
}

void test_runtime_dependency_capture_deduplicates_repeated_binding_reads() {
  using namespace amber::bytecode;

  BcModule module;
  module.strings = {"x"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool = {one};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Module;
  code.reg_count = 3;
  code.local_layout.push_back({0, 0, 0, 0});
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::WatchLocal, {{1, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Move, {{2, false}, {0, false}}});
  code.instructions.push_back({Opcode::Move, {{2, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  world.begin_dependency_capture(7);
  const amber::runtime::ExecutionResult exec = world.execute(1);
  expect(exec.ok(), "repeated binding dependency module should execute");
  const amber::runtime::RuntimeDependencySet deps =
      world.end_dependency_capture();
  expect(deps.notebook_cell_id == 7,
         "repeated binding capture keeps notebook cell id");
  expect(deps.dependencies.size() == 1,
         "repeated binding reads are captured once");
  expect(deps.dependencies[0].kind ==
             amber::runtime::RuntimeDependencyKind::Binding,
         "repeated dependency is a binding dependency");
  expect(deps.dependencies[0].cell_id == exec.watch_events[0].cell_id,
         "deduplicated dependency keeps binding id");
}

void test_runtime_dependency_capture_records_nested_ivar_reads() {
  const amber::bytecode::EmitResult emit_result =
      emit_ok("class Particle:\n"
              "  def observe():\n"
              "    Kernel.watch(@mass)\n"
              "    @mass = 7\n"
              "    @mass\n"
              "\n"
              "def probe():\n"
              "  Particle().observe()\n");
  const amber::bytecode::DecodeResult decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(emit_result.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  const amber::bytecode::BcMethod *probe =
      method_by_name(decoded.module, "probe");
  expect(probe != nullptr, "dependency capture ivar probe method exists");

  amber::runtime::RuntimeWorld world(decoded.module);
  world.begin_dependency_capture(99);
  const amber::runtime::ExecutionResult exec =
      world.execute(probe->entry_code_id);
  expect(exec.ok(), "dependency capture ivar module should execute");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "dependency capture ivar returns watched field value");
  const amber::runtime::RuntimeDependencySet deps =
      world.end_dependency_capture();
  expect(deps.notebook_cell_id == 99,
         "ivar dependency capture keeps notebook cell id");
  expect(deps.dependencies.size() == 1,
         "ivar dependency capture records one field dependency");

  const amber::runtime::RuntimeDependency *ivar =
      dependency_by_kind(deps, amber::runtime::RuntimeDependencyKind::Ivar);
  expect(ivar != nullptr, "dependency capture exposes ivar dependency");
  expect(ivar->object_id == exec.watch_events[0].object_id,
         "ivar dependency uses watch object id");
  expect(ivar->target_name == "@mass" && ivar->field_name == "mass",
         "ivar dependency keeps field target");
  expect(ivar->revision == 1 && ivar->object_revision == 1,
         "ivar dependency records current field and object revisions");
  expect(ivar->cell_id == 0, "ivar dependency does not set binding cell id");
}

void test_runtime_sequence_collections_contract() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"lazy",     "map",   "select", "reduce", "+",     ">",
                    "flat_map", "group", "count",  "find",   "first", "to_array",
                    "any?",     "all?",  "none?",  "low",    "high"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  Constant low;
  low.kind = ConstantKind::SymbolRef;
  low.ref_id = symbol_id_or_die(module, "low");
  Constant high;
  high.kind = ConstantKind::SymbolRef;
  high.ref_id = symbol_id_or_die(module, "high");
  module.const_pool = {one, low, high};

  BcCode chain;
  chain.code_id = 1;
  chain.kind = CodeKind::Method;
  chain.reg_count = 8;
  chain.instructions.push_back(
      send_instr(4, 0, symbol_id_or_die(module, "lazy")));
  chain.instructions.push_back(
      send_instr(5, 4, symbol_id_or_die(module, "map"), {}, 1, 1));
  chain.instructions.push_back(
      send_instr(6, 5, symbol_id_or_die(module, "select"), {}, 2, 2));
  chain.instructions.push_back(
      send_instr(7, 6, symbol_id_or_die(module, "reduce"), {}, 3, 3));
  chain.instructions.push_back({Opcode::Return, {{7, false}}});

  BcCode empty_reduce;
  empty_reduce.code_id = 2;
  empty_reduce.kind = CodeKind::Method;
  empty_reduce.reg_count = 3;
  empty_reduce.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "reduce"), {}, 1));
  empty_reduce.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode flat_map;
  flat_map.code_id = 3;
  flat_map.kind = CodeKind::Method;
  flat_map.reg_count = 3;
  flat_map.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "flat_map"), {}, 1));
  flat_map.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode group_by;
  group_by.code_id = 4;
  group_by.kind = CodeKind::Method;
  group_by.reg_count = 3;
  group_by.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "group"), {}, 1));
  group_by.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode count_find;
  count_find.code_id = 5;
  count_find.kind = CodeKind::Method;
  count_find.reg_count = 5;
  count_find.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "count"), {}, 1));
  count_find.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "find"), {}, 1));
  count_find.instructions.push_back(
      {Opcode::MakeList, {{4, false}, {2, false}, {2, false}}});
  count_find.instructions.push_back({Opcode::Return, {{4, false}}});

  BcCode reduce_init;
  reduce_init.code_id = 6;
  reduce_init.kind = CodeKind::Method;
  reduce_init.reg_count = 4;
  reduce_init.instructions.push_back({Opcode::LoadK, {{2, false}, {0, false}}});
  reduce_init.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "reduce"), {2}, 1));
  reduce_init.instructions.push_back({Opcode::Return, {{3, false}}});

  BcCode shape_probe;
  shape_probe.code_id = 7;
  shape_probe.kind = CodeKind::Method;
  shape_probe.reg_count = 11;
  shape_probe.instructions.push_back({Opcode::LoadK, {{2, false}, {0, false}}});
  shape_probe.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "first")));
  shape_probe.instructions.push_back(
      send_instr(4, 0, symbol_id_or_die(module, "first"), {2}));
  shape_probe.instructions.push_back(
      send_instr(5, 0, symbol_id_or_die(module, "to_array")));
  shape_probe.instructions.push_back(
      send_instr(6, 0, symbol_id_or_die(module, "any?")));
  shape_probe.instructions.push_back(
      send_instr(7, 0, symbol_id_or_die(module, "all?")));
  shape_probe.instructions.push_back(
      send_instr(8, 0, symbol_id_or_die(module, "none?")));
  shape_probe.instructions.push_back(
      {Opcode::MakeList, {{9, false}, {3, false}, {6, false}}});
  shape_probe.instructions.push_back({Opcode::Return, {{9, false}}});

  BcCode inc;
  inc.code_id = 10;
  inc.kind = CodeKind::Block;
  inc.reg_count = 3;
  inc.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  inc.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "+"), {1}));
  inc.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode gt_one;
  gt_one.code_id = 11;
  gt_one.kind = CodeKind::Block;
  gt_one.reg_count = 3;
  gt_one.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  gt_one.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, ">"), {1}));
  gt_one.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode add;
  add.code_id = 12;
  add.kind = CodeKind::Block;
  add.reg_count = 3;
  add.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "+"), {1}));
  add.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode pairify;
  pairify.code_id = 13;
  pairify.kind = CodeKind::Block;
  pairify.reg_count = 3;
  pairify.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  pairify.instructions.push_back(
      send_instr(1, 0, symbol_id_or_die(module, "+"), {1}));
  pairify.instructions.push_back(
      {Opcode::MakeList, {{2, false}, {0, false}, {2, false}}});
  pairify.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode low_high_key;
  low_high_key.code_id = 14;
  low_high_key.kind = CodeKind::Block;
  low_high_key.reg_count = 5;
  low_high_key.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {0, false}}});
  low_high_key.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, ">"), {1}));
  low_high_key.instructions.push_back(
      {Opcode::JumpIfFalse, {{2, false}, {5, false}}});
  low_high_key.instructions.push_back(
      {Opcode::LoadK, {{3, false}, {2, false}}});
  low_high_key.instructions.push_back({Opcode::Return, {{3, false}}});
  low_high_key.instructions.push_back(
      {Opcode::LoadK, {{4, false}, {1, false}}});
  low_high_key.instructions.push_back({Opcode::Return, {{4, false}}});

  module.code_objects = {chain,      empty_reduce, flat_map,    group_by,
                         count_find, reduce_init,  shape_probe, inc,
                         gt_one,     add,          pairify,     low_high_key};

  const amber::runtime::Value source = amber::runtime::make_list_value(
      {amber::runtime::Value::integer(0), amber::runtime::Value::integer(1),
       amber::runtime::Value::integer(2)});
  const amber::runtime::ExecutionResult chained = amber::runtime::execute_code(
      module, 1,
      {source, make_closure_value(10), make_closure_value(11),
       make_closure_value(12)});
  expect(chained.ok(), "lazy/map/select/reduce chain should execute");
  expect(chained.value.is_integer() && chained.value.as_integer() == 5,
         "lazy/map/select/reduce should produce deterministic eager result");

  const amber::runtime::ExecutionResult empty = amber::runtime::execute_code(
      module, 2, {amber::runtime::make_list_value({}), make_closure_value(12)});
  expect(!empty.ok() && empty.fault.has_value() &&
             empty.fault->error_name == "EmptyCollectionError",
         "empty reduce without init should raise EmptyCollectionError");

  const amber::runtime::ExecutionResult flattened =
      amber::runtime::execute_code(
          module, 3,
          {amber::runtime::make_list_value({amber::runtime::Value::integer(1),
                                            amber::runtime::Value::integer(2)}),
           make_closure_value(13)});
  expect(flattened.ok(), "flat_map should execute");
  expect_integer_list(flattened.value, {1, 2, 2, 3}, "flat_map");

  const amber::runtime::ExecutionResult counted =
      amber::runtime::execute_code(module, 5, {source, make_closure_value(11)});
  expect(counted.ok(), "count/find should execute");
  expect_integer_list(counted.value, {1, 2}, "count/find");

  const amber::runtime::ExecutionResult reduced_with_init =
      amber::runtime::execute_code(module, 6, {source, make_closure_value(12)});
  expect(reduced_with_init.ok() && reduced_with_init.value.is_integer() &&
             reduced_with_init.value.as_integer() == 4,
         "reduce(init) should start from explicit accumulator");

  const amber::runtime::ExecutionResult shaped =
      amber::runtime::execute_code(module, 7, {source});
  expect(shaped.ok() && shaped.value.is_list(),
         "first/to_a/predicate probe should execute");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> shape_parts =
      shaped.value.as_list();
  expect(shape_parts != nullptr && shape_parts->items.size() == 6,
         "first/to_a/predicate probe shape");
  expect(shape_parts->items[0].is_integer() &&
             shape_parts->items[0].as_integer() == 0,
         "first should return first item");
  expect_integer_list(shape_parts->items[1], {0}, "first(count)");
  expect_integer_list(shape_parts->items[2], {0, 1, 2}, "to_array");
  expect(shape_parts->items[3].is_bool() && shape_parts->items[3].as_bool(),
         "any? should see truthy items");
  expect(shape_parts->items[4].is_bool() && shape_parts->items[4].as_bool(),
         "all? should see all truthy items");
  expect(shape_parts->items[5].is_bool() && !shape_parts->items[5].as_bool(),
         "none? should reject truthy items");

  const amber::runtime::ExecutionResult grouped =
      amber::runtime::execute_code(module, 4, {source, make_closure_value(14)});
  expect(grouped.ok(), "group_by should execute");
  expect(grouped.value.is_map(), "group_by should return map");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> groups =
      grouped.value.as_map();
  expect(groups != nullptr && groups->entries.size() == 2,
         "group_by should preserve first-key ordering");
  expect(groups->entries[0].symbol_id == symbol_id_or_die(module, "low"),
         "group_by low key first");
  expect_integer_list(groups->entries[0].value, {0, 1}, "group_by low");
  expect(groups->entries[1].symbol_id == symbol_id_or_die(module, "high"),
         "group_by high key second");
  expect_integer_list(groups->entries[1].value, {2}, "group_by high");
}

void test_runtime_map_collections_contract() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"keys",
                    "values",
                    "entries",
                    "select",
                    "reject",
                    "map",
                    "transform_values",
                    "each",
                    "+",
                    ">",
                    "alpha",
                    "beta"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool = {one};

  BcCode probe;
  probe.code_id = 1;
  probe.kind = CodeKind::Method;
  probe.reg_count = 12;
  probe.instructions.push_back(
      send_instr(4, 0, symbol_id_or_die(module, "keys")));
  probe.instructions.push_back(
      send_instr(5, 0, symbol_id_or_die(module, "values")));
  probe.instructions.push_back(
      send_instr(6, 0, symbol_id_or_die(module, "entries")));
  probe.instructions.push_back(
      send_instr(7, 0, symbol_id_or_die(module, "select"), {}, 1));
  probe.instructions.push_back(
      send_instr(8, 0, symbol_id_or_die(module, "reject"), {}, 1));
  probe.instructions.push_back(
      send_instr(9, 0, symbol_id_or_die(module, "transform_values"), {}, 2));
  probe.instructions.push_back(
      send_instr(10, 0, symbol_id_or_die(module, "map"), {}, 3));
  probe.instructions.push_back(
      {Opcode::MakeList, {{11, false}, {4, false}, {7, false}}});
  probe.instructions.push_back({Opcode::Return, {{11, false}}});

  BcCode each_probe;
  each_probe.code_id = 2;
  each_probe.kind = CodeKind::Method;
  each_probe.reg_count = 3;
  each_probe.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "each"), {}, 1));
  each_probe.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode map_value_gt_one;
  map_value_gt_one.code_id = 20;
  map_value_gt_one.kind = CodeKind::Block;
  map_value_gt_one.reg_count = 4;
  map_value_gt_one.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {0, false}}});
  map_value_gt_one.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, ">"), {2}));
  map_value_gt_one.instructions.push_back({Opcode::Return, {{3, false}}});

  BcCode inc_value;
  inc_value.code_id = 21;
  inc_value.kind = CodeKind::Block;
  inc_value.reg_count = 3;
  inc_value.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  inc_value.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "+"), {1}));
  inc_value.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode map_pair_value;
  map_pair_value.code_id = 22;
  map_pair_value.kind = CodeKind::Block;
  map_pair_value.reg_count = 4;
  map_pair_value.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {0, false}}});
  map_pair_value.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, "+"), {2}));
  map_pair_value.instructions.push_back({Opcode::Return, {{3, false}}});

  module.code_objects = {probe, each_probe, map_value_gt_one, inc_value,
                         map_pair_value};

  const amber::runtime::Value map =
      make_symbol_map(module, {{"alpha", amber::runtime::Value::integer(1)},
                               {"beta", amber::runtime::Value::integer(2)}});
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1,
      {map, make_closure_value(20), make_closure_value(21),
       make_closure_value(22)});
  expect(exec.ok(), "map collections probe should execute");
  expect(exec.value.is_list(), "map collections probe should return list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> parts =
      exec.value.as_list();
  expect(parts != nullptr && parts->items.size() == 7,
         "map collections probe should return seven parts");
  expect(parts->items[0].is_list() &&
             parts->items[0].as_list()->items[0].as_symbol().symbol_id ==
                 symbol_id_or_die(module, "alpha") &&
             parts->items[0].as_list()->items[1].as_symbol().symbol_id ==
                 symbol_id_or_die(module, "beta"),
         "Map#keys should preserve entry order");
  expect_integer_list(parts->items[1], {1, 2}, "Map#values");
  expect(parts->items[2].is_list() &&
             parts->items[2].as_list()->items.size() == 2 &&
             parts->items[2].as_list()->items[0].is_tuple(),
         "Map#entries should return key/value tuples");
  expect(parts->items[3].is_map() &&
             parts->items[3].as_map()->entries.size() == 1 &&
             parts->items[3].as_map()->entries[0].symbol_id ==
                 symbol_id_or_die(module, "beta"),
         "Map#select should keep matching entries in order");
  expect(parts->items[4].is_map() &&
             parts->items[4].as_map()->entries.size() == 1 &&
             parts->items[4].as_map()->entries[0].symbol_id ==
                 symbol_id_or_die(module, "alpha"),
         "Map#reject should keep non-matching entries in order");
  expect(parts->items[5].is_map() &&
             parts->items[5].as_map()->entries[0].value.as_integer() == 2 &&
             parts->items[5].as_map()->entries[1].value.as_integer() == 3,
         "Map#transform_values should preserve keys and transform values");
  expect_integer_list(parts->items[6], {2, 3}, "Map#map");

  const amber::runtime::ExecutionResult each =
      amber::runtime::execute_code(module, 2, {map, make_closure_value(22)});
  expect(each.ok() && each.value.is_map() &&
             each.value.as_map() == map.as_map(),
         "Map#each should return the receiver after visiting entries");
}

void test_manual_instance_send_dispatch() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Particle", "mass"};
  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  BcClass klass;
  klass.class_name_sym_id = 0;
  klass.method_range_start = 0;
  klass.method_range_count = 1;
  module.classes.push_back(klass);

  BcMethod method;
  method.selector_sym_id = 1;
  method.owner_dispatch_ref = 0;
  method.signature_blob_id = 0;
  method.entry_code_id = 2;
  method.flags = 1;
  module.methods.push_back(method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {1, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 2;
  body.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  body.instructions.push_back(
      {Opcode::LoadIvar, {{1, false}, {0, false}, {1, false}, {0, false}}});
  body.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {caller, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->ivars["mass"] = amber::runtime::Value::integer(11);

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "instance send dispatch failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 11,
         "instance send should dispatch to local method table");
}

void test_manual_store_and_load_ivar() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{1, false}, {0, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{2, false}, {1, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(17)},
      amber::runtime::Value::instance(instance));
  expect(exec.ok(), "ivar store/load execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 17,
         "ivar load should return stored value");
  expect(instance->ivars.count("mass") == 1,
         "ivar slot should be materialized");
  expect(instance->ivars.at("mass").is_integer() &&
             instance->ivars.at("mass").as_integer() == 17,
         "ivar map should contain stored integer");
}

void test_manual_store_and_load_cvar() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"ρ"};
  module.classes.push_back(BcClass{});

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  code.instructions.push_back(
      {Opcode::StoreCvar, {{1, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::LoadCvar, {{2, false}, {1, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(19)},
      amber::runtime::Value::class_object(0));
  expect(exec.ok(), "cvar store/load execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 19,
         "cvar load should return stored value");
}

void test_manual_multi_segment_lookup_const() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"physics", "Particle"};

  Constant particle_path;
  particle_path.kind = ConstantKind::Path;
  particle_path.items = {0, 1};
  module.const_pool.push_back(particle_path);

  BcClass particle;
  particle.class_name_sym_id = 1;
  module.classes.push_back(particle);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({Opcode::LookupConst, {{0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects.push_back(code);

  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1);
  expect(exec.ok(), "multi-segment LOOKUP_CONST failed");
  expect(exec.value.is_class_object() &&
             exec.value.as_class_object().class_index == 0,
         "multi-segment LOOKUP_CONST should resolve class leaf");
}

void test_manual_multi_segment_superclass_dispatch() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"physics", "Base", "Child", "answer"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant base_path;
  base_path.kind = ConstantKind::Path;
  base_path.items = {0, 1};
  module.const_pool.push_back(base_path);

  Constant answer_value;
  answer_value.kind = ConstantKind::Integer;
  answer_value.int_value = 41;
  module.const_pool.push_back(answer_value);

  BcClass base;
  base.class_name_sym_id = 1;
  base.method_range_start = 0;
  base.method_range_count = 1;
  module.classes.push_back(base);

  BcClass child;
  child.class_name_sym_id = 2;
  child.has_superclass_ref = true;
  child.superclass_ref = 1;
  child.method_range_start = 1;
  child.method_range_count = 0;
  module.classes.push_back(child);

  BcMethod method;
  method.selector_sym_id = 3;
  method.owner_dispatch_ref = 0;
  method.signature_blob_id = 0;
  method.entry_code_id = 2;
  method.flags = 1;
  module.methods.push_back(method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {3, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 1;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "multi-segment superclass dispatch failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 41,
         "multi-segment superclass ref should resolve through dispatch");
}

void test_manual_send_cache_receiver_class_guard() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"A", "B", "value", "+", "=="};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant zero;
  zero.kind = ConstantKind::Integer;
  zero.int_value = 0;
  module.const_pool.push_back(zero);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcClass class_a;
  class_a.class_name_sym_id = 0;
  class_a.method_range_start = 0;
  class_a.method_range_count = 1;
  module.classes.push_back(class_a);

  BcClass class_b;
  class_b.class_name_sym_id = 1;
  class_b.method_range_start = 1;
  class_b.method_range_count = 1;
  module.classes.push_back(class_b);

  BcMethod method_a;
  method_a.selector_sym_id = 2;
  method_a.owner_dispatch_ref = 0;
  method_a.signature_blob_id = 0;
  method_a.entry_code_id = 2;
  method_a.flags = 1;
  module.methods.push_back(method_a);

  BcMethod method_b;
  method_b.selector_sym_id = 2;
  method_b.owner_dispatch_ref = 1;
  method_b.signature_blob_id = 0;
  method_b.entry_code_id = 3;
  method_b.flags = 1;
  module.methods.push_back(method_b);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 6;
  caller.instructions.push_back({Opcode::LoadK, {{3, false}, {1, false}}});
  caller.instructions.push_back({Opcode::LoadK, {{4, false}, {2, false}}});
  caller.instructions.push_back({Opcode::Send,
                                 {{2, false},
                                  {0, false},
                                  {2, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Send,
                                 {{3, false},
                                  {3, false},
                                  {3, false},
                                  {1, false},
                                  {4, false},
                                  {0, false},
                                  {-1, true},
                                  {1, false}}});
  caller.instructions.push_back({Opcode::Send,
                                 {{5, false},
                                  {3, false},
                                  {4, false},
                                  {1, false},
                                  {4, false},
                                  {0, false},
                                  {-1, true},
                                  {2, false}}});
  caller.instructions.push_back(
      {Opcode::JumpIfFalse, {{5, false}, {8, false}}});
  caller.instructions.push_back({Opcode::Move, {{0, false}, {1, false}}});
  caller.instructions.push_back({Opcode::Jump, {{2, false}}});
  caller.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode body_a;
  body_a.code_id = 2;
  body_a.kind = CodeKind::Method;
  body_a.reg_count = 1;
  body_a.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  body_a.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_b;
  body_b.code_id = 3;
  body_b.kind = CodeKind::Method;
  body_b.reg_count = 1;
  body_b.instructions.push_back({Opcode::LoadK, {{0, false}, {3, false}}});
  body_b.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_a, body_b};

  auto a = amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  a->class_index = 0;
  auto b = amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  b->class_index = 1;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1,
      {amber::runtime::Value::instance(a), amber::runtime::Value::instance(b)});
  expect(exec.ok(), "send cache receiver-class guard execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 2,
         "send cache should miss when receiver class changes");
}

void test_manual_ivar_cache_shape_guard() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadSelf, {{1, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{2, false}, {1, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{1, false}, {0, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{2, false}, {1, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(code);

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::integer(23)},
      amber::runtime::Value::instance(instance));
  expect(exec.ok(), "ivar cache shape guard execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 23,
         "ivar cache should miss after a shape-changing store");
}

void test_runtime_ivar_shape_slot_transition_stability() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass", "charge"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 4;
  code.instructions.push_back({Opcode::LoadSelf, {{2, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{2, false}, {0, false}, {0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::StoreIvar, {{2, false}, {1, false}, {1, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{3, false}, {2, false}, {0, false}, {2, false}}});
  code.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  auto first = amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  first->class_index = 0;
  const amber::runtime::ExecutionResult first_exec = world.execute(
      1,
      {amber::runtime::Value::integer(7), amber::runtime::Value::integer(11)},
      amber::runtime::Value::instance(first));
  expect(first_exec.ok(), "first shape transition execution failed");
  expect(first_exec.value.is_integer() && first_exec.value.as_integer() == 7,
         "shape transition test should read stored mass");
  expect(first->header.shape != nullptr && !first->header.shape->dead,
         "instance should have a live runtime shape");
  expect(first->header.shape->slot_names.size() == 2,
         "shape should allocate two ivar slots");
  expect(first->header.shape->ivar_slots.at("mass") == 0,
         "mass should occupy first slot");
  expect(first->header.shape->ivar_slots.at("charge") == 1,
         "charge should occupy second slot");
  expect(first->ivar_storage.size() == 2, "ivar storage should be slot-backed");
  expect(first->ivar_storage[0].is_integer() &&
             first->ivar_storage[0].as_integer() == 7,
         "mass slot should contain first argument");
  expect(first->ivar_storage[1].is_integer() &&
             first->ivar_storage[1].as_integer() == 11,
         "charge slot should contain second argument");
  expect(first->ivar_shape_version == first->header.shape->shape_version,
         "legacy shape version mirror should track shape descriptor");
  const std::uint64_t final_shape_id = first->header.shape->shape_id;
  const std::uint64_t final_shape_version = first->header.shape->shape_version;

  auto second = amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  second->class_index = 0;
  const amber::runtime::ExecutionResult second_exec = world.execute(
      1, {amber::runtime::Value::integer(5), amber::runtime::Value::integer(6)},
      amber::runtime::Value::instance(second));
  expect(second_exec.ok(), "second shape transition execution failed");
  expect(second->header.shape != nullptr &&
             second->header.shape->shape_id == final_shape_id,
         "same ivar growth path should reuse the same final shape");
  expect(second->header.shape->shape_version == final_shape_version,
         "reused shape should keep a stable shape version");

  const amber::runtime::ExecutionResult update_exec = world.execute(
      1,
      {amber::runtime::Value::integer(13), amber::runtime::Value::integer(17)},
      amber::runtime::Value::instance(first));
  expect(update_exec.ok(), "existing-slot store execution failed");
  expect(first->header.shape->shape_id == final_shape_id,
         "storing existing ivars should not transition shape");
  expect(first->ivar_storage[0].is_integer() &&
             first->ivar_storage[0].as_integer() == 13,
         "existing mass slot should be overwritten in place");
  expect(first->ivars.at("charge").is_integer() &&
             first->ivars.at("charge").as_integer() == 17,
         "legacy ivar map mirror should stay synchronized");
}

void test_runtime_dead_shape_rejects_ivar_access() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  code.instructions.push_back(
      {Opcode::LoadIvar, {{1, false}, {0, false}, {0, false}, {0, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  auto dead_shape = std::make_shared<amber::runtime::ShapeDescriptor>();
  dead_shape->dead = true;
  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.shape = dead_shape;
  instance->header.flags = amber::runtime::kObjectFlagDead;

  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {}, amber::runtime::Value::instance(instance));
  expect(!exec.ok(), "dead-shape ivar load should fail");
  expect(exec.fault.has_value() &&
             exec.fault->error_name == "UseAfterFreeError",
         "dead-shape ivar load should report UseAfterFreeError");
}

void test_runtime_heap_worker_arena_headers() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value list = amber::runtime::Value::null();
  amber::runtime::Value tuple = amber::runtime::Value::null();
  amber::runtime::Value map = amber::runtime::Value::null();
  amber::runtime::IntrusivePtr<amber::runtime::InstanceValue> instance;
  amber::runtime::IntrusivePtr<amber::runtime::ClosureValue> closure;
  {
    amber::runtime::RuntimeWorkerScope worker(7);
    list = heap.make_list_value({amber::runtime::Value::integer(1)});
    tuple = heap.make_tuple_value({amber::runtime::Value::integer(2)});
    map = heap.make_symbol_map_value({{0, amber::runtime::Value::integer(3)}},
                                     true);
    instance = heap.make_instance_value(4);
    closure = heap.make_closure_value();
  }

  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list_ptr =
      list.as_list();
  expect(list_ptr->header.allocation_id != 0,
         "list should carry heap allocation id");
  expect(list_ptr->header.arena_worker_id == 7,
         "list should record owner arena worker");
  expect(tuple.as_tuple()->header.arena_worker_id == 7,
         "tuple should record owner arena worker");
  expect(map.as_map()->header.owner.kind ==
             amber::runtime::OwnerTokenKind::Shareable,
         "frozen map should remain shareable");
  expect(instance->header.kind == amber::runtime::HeapObjectKind::Instance &&
             instance->header.class_index == 4,
         "allocator should initialize instance header");
  expect(closure->header.kind == amber::runtime::HeapObjectKind::Closure,
         "allocator should initialize closure header");

  const amber::runtime::RuntimeHeapStats stats = heap.stats();
  const amber::runtime::RuntimeArenaStats *arena = arena_stats_for(stats, 7);
  expect(arena != nullptr, "worker arena stats should exist");
  expect(arena->allocations == 5 && arena->live_objects == 5,
         "worker arena should count live allocations");
  expect(stats.instance_allocations == 1 && stats.array_allocations == 2 &&
             stats.map_allocations == 1 && stats.closure_allocations == 1,
         "heap stats should split allocations by runtime object family");
}

void test_runtime_heap_remote_free_queue_drains_on_owner() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value = amber::runtime::Value::null();
  {
    amber::runtime::RuntimeWorkerScope owner(1);
    value = heap.make_list_value({amber::runtime::Value::integer(9)});
  }

  {
    amber::runtime::RuntimeWorkerScope other_worker(2);
    value = amber::runtime::Value::null();
  }

  amber::runtime::RuntimeHeapStats queued = heap.stats();
  expect(queued.live_objects == 1, "remote free should retain object memory");
  expect(queued.remote_frees_queued == 1,
         "remote free should be queued for owner arena");
  expect(queued.remote_queue_depth == 1,
         "remote queue depth should track queued free");
  const amber::runtime::RuntimeArenaStats *owner_arena =
      arena_stats_for(queued, 1);
  expect(owner_arena != nullptr && owner_arena->remote_queue_depth == 1,
         "owner arena should own the remote-free queue");

  const std::uint64_t drained = heap.drain_remote_frees(1);
  expect(drained == 1, "owner drain should free one queued object");
  const amber::runtime::RuntimeHeapStats drained_stats = heap.stats();
  expect(drained_stats.live_objects == 0,
         "drained remote free should release memory");
  expect(drained_stats.remote_frees_drained == 1,
         "remote drain count should be recorded");
  expect(drained_stats.remote_queue_depth == 0,
         "remote queue should be empty after drain");
}

void test_runtime_heap_allocation_heavy_smoke() {
  amber::runtime::RuntimeHeap heap;
  {
    amber::runtime::RuntimeWorkerScope worker(3);
    std::vector<amber::runtime::Value> values;
    values.reserve(4096);
    for (std::int64_t i = 0; i < 4096; ++i) {
      values.push_back(
          heap.make_list_value({amber::runtime::Value::integer(i),
                                amber::runtime::Value::integer(i + 1)}));
    }
    const amber::runtime::RuntimeHeapStats live_stats = heap.stats();
    expect(live_stats.allocations == 4096 && live_stats.live_objects == 4096,
           "allocation-heavy smoke should retain all live lists");
    values.clear();
  }

  const amber::runtime::RuntimeHeapStats stats = heap.stats();
  expect(stats.allocations == 4096 && stats.live_objects == 0,
         "allocation-heavy smoke should free all local lists");
  expect(stats.local_frees == 4096,
         "allocation-heavy smoke should use local arena frees");
}

void test_runtime_gc_full_cycle_preserves_root_address() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value root =
      heap.make_list_value({amber::runtime::Value::integer(1)});
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> root_ptr =
      root.as_list();
  const amber::runtime::ListValue *address = root_ptr.get();

  const amber::runtime::RuntimeGcResult result =
      heap.collect_garbage({root}, amber::runtime::RuntimeGcCycle::Full);
  expect(result.marked == 1, "full GC should mark the rooted list");
  expect(result.reclaimed == 0, "full GC should not reclaim rooted list");
  expect(root.as_list().get() == address,
         "non-moving GC should preserve object address");
  expect(root_ptr->header.generation ==
             amber::runtime::ObjectGeneration::Mature,
         "rooted young object should promote after surviving GC");
  expect(root_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "rooted object should remain live after GC");
}

void test_runtime_gc_reclaims_unrooted_reference_cycle() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value left = heap.make_list_value({});
  amber::runtime::Value right = heap.make_list_value({});
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> left_ptr =
      left.as_list();
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> right_ptr =
      right.as_list();
  left_ptr->items.push_back(right);
  right_ptr->items.push_back(left);

  const amber::runtime::RuntimeGcResult result =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
  expect(result.reclaimed == 2,
         "full GC should reclaim an unrooted heap reference cycle");
  expect(left_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "left cycle node should become a GC tombstone");
  expect(right_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "right cycle node should become a GC tombstone");
  expect(left_ptr->items.empty() && right_ptr->items.empty(),
         "GC tombstone rewrite should sever outgoing references");
  expect(heap.stats().live_objects == 0,
         "GC logical reclaim should remove cycle nodes from live stats");
}

void test_runtime_gc_write_barrier_remembers_mature_to_young_edge() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value parent = heap.make_list_value({});
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> parent_ptr =
      parent.as_list();

  heap.collect_garbage({parent}, amber::runtime::RuntimeGcCycle::Full);
  expect(parent_ptr->header.generation ==
             amber::runtime::ObjectGeneration::Mature,
         "parent should be mature before remembered-set probe");

  amber::runtime::Value child =
      heap.make_list_value({amber::runtime::Value::integer(7)});
  const amber::runtime::RuntimeWriteBarrierResult barrier =
      heap.write_barrier(parent, child);
  expect(barrier.ok && barrier.remembered,
         "mature-to-young write should update remembered set");
  parent_ptr->items.push_back(child);
  expect(heap.stats().remembered_set_entries == 1,
         "remembered set should expose one mature-to-young edge");

  const amber::runtime::RuntimeGcResult young =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Young);
  expect(young.reclaimed == 0,
         "young GC should retain child reachable from remembered set");
  expect(child.as_list()->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "remembered child should remain live");
}

void test_runtime_gc_write_barrier_rejects_invalid_edges() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value shared_owner = heap.make_tuple_value({});
  amber::runtime::Value confined_child = heap.make_list_value({});

  const amber::runtime::RuntimeWriteBarrierResult isolation =
      heap.write_barrier(shared_owner, confined_child);
  expect(!isolation.ok && isolation.error_name == "IsolationError",
         "shared-to-confined write should fail isolation barrier");

  amber::runtime::Value owner = heap.make_list_value({});
  confined_child.as_list()->header.lifetime_state =
      amber::runtime::ObjectLifetimeState::Deallocated;
  confined_child.as_list()->header.flags |= amber::runtime::kObjectFlagDead;
  const amber::runtime::RuntimeWriteBarrierResult lifetime =
      heap.write_barrier(owner, confined_child);
  expect(!lifetime.ok && lifetime.error_name == "UseAfterFreeError",
         "write barrier should reject deallocated heap references");

  const amber::runtime::RuntimeHeapStats stats = heap.stats();
  expect(stats.write_barrier_rejected_isolation == 1,
         "barrier stats should count isolation rejects");
  expect(stats.write_barrier_rejected_lifetime == 1,
         "barrier stats should count lifetime rejects");
}

void test_runtime_gc_safepoint_scans_vm_frame_roots() {
  using namespace amber::bytecode;

  BcModule module;
  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::MakeList, {{1, false}, {0, false}, {1, false}}});
  code.instructions.push_back({Opcode::Safepoint, {}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  world.request_garbage_collection(amber::runtime::RuntimeGcCycle::Full);
  const amber::runtime::ExecutionResult exec = world.execute(1);
  expect(exec.ok(), "safepoint GC probe should execute");
  expect(exec.value.is_list(), "safepoint GC probe should return list");
  expect(exec.value.as_list()->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "safepoint GC should preserve live frame register root");

  const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
  expect(stats.gc_safepoint_collections == 1,
         "safepoint should run one requested GC cycle");
  expect(stats.gc_full_cycles == 1,
         "requested safepoint GC should be a full cycle");
}

void test_runtime_gc_safepoint_preserves_caller_roots_during_call() {
  using namespace amber::bytecode;

  BcModule module;
  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 4;
  caller.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  caller.instructions.push_back(
      {Opcode::MakeList, {{1, false}, {0, false}, {1, false}}});
  caller.instructions.push_back(
      {Opcode::MakeClosure, {{2, false}, {2, false}, {0, false}}});
  caller.instructions.push_back({Opcode::Call,
                                 {{3, false},
                                  {2, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode callee;
  callee.code_id = 2;
  callee.kind = CodeKind::Block;
  callee.reg_count = 1;
  callee.instructions.push_back({Opcode::Safepoint, {}});
  callee.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  callee.instructions.push_back({Opcode::Return, {{0, false}}});

  module.code_objects = {caller, callee};

  amber::runtime::RuntimeWorld world(module);
  world.request_garbage_collection(amber::runtime::RuntimeGcCycle::Full);
  const amber::runtime::ExecutionResult exec = world.execute(1);
  expect(exec.ok(), "call-boundary safepoint probe should execute");
  expect(exec.value.is_list(),
         "caller root should be returned after callee GC");
  expect(exec.value.as_list()->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "callee safepoint GC should preserve caller frame roots");
  const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
  expect(stats.gc_safepoint_collections == 1,
         "callee safepoint should run requested GC once");
  expect(stats.gc_reclaimed_objects == 0,
         "callee safepoint should not reclaim caller live roots");
}

void test_runtime_gc_backedge_safepoint_preserves_live_roots() {
  using namespace amber::bytecode;

  BcModule module;
  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::MakeList, {{1, false}, {0, false}, {1, false}}});
  code.instructions.push_back({Opcode::LoadBool, {{2, false}, {1, false}}});
  code.instructions.push_back({Opcode::Safepoint, {}});
  code.instructions.push_back({Opcode::JumpIfFalse, {{2, false}, {7, false}}});
  code.instructions.push_back({Opcode::LoadBool, {{2, false}, {0, false}}});
  code.instructions.push_back({Opcode::Jump, {{3, false}}});
  code.instructions.push_back({Opcode::Return, {{1, false}}});
  code.safepoint_table.push_back({3, 0});
  module.code_objects.push_back(code);

  amber::runtime::RuntimeWorld world(module);
  world.request_garbage_collection(amber::runtime::RuntimeGcCycle::Full);
  const amber::runtime::ExecutionResult exec = world.execute(1);
  expect(exec.ok(), "backedge safepoint probe should execute");
  expect(exec.value.is_list(), "loop root should be returned after backedge");
  expect(exec.value.as_list()->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "backedge safepoint GC should preserve live loop root");
  const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
  expect(stats.gc_safepoint_collections == 1,
         "loop safepoint should consume one GC request");
  expect(stats.gc_reclaimed_objects == 0,
         "loop safepoint should not reclaim live loop roots");
}

void test_runtime_gc_preserves_rooted_local_and_shared_cycles() {
  amber::runtime::RuntimeHeap heap;

  amber::runtime::Value local_left = heap.make_list_value({});
  amber::runtime::Value local_right = heap.make_list_value({});
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> local_left_ptr =
      local_left.as_list();
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> local_right_ptr =
      local_right.as_list();
  local_left_ptr->items.push_back(local_right);
  local_right_ptr->items.push_back(local_left);

  const amber::runtime::RuntimeGcResult rooted_local =
      heap.collect_garbage({local_left}, amber::runtime::RuntimeGcCycle::Full);
  expect(rooted_local.marked == 2 && rooted_local.reclaimed == 0,
         "full GC should preserve a rooted local reference cycle");
  expect(local_left_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "rooted local cycle left node should stay live");
  expect(local_right_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "rooted local cycle right node should stay live");

  local_left = amber::runtime::Value::null();
  local_right = amber::runtime::Value::null();
  const amber::runtime::RuntimeGcResult unrooted_local =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
  expect(unrooted_local.reclaimed == 2,
         "full GC should reclaim local cycle after roots clear");

  amber::runtime::Value shared_left = heap.make_tuple_value({});
  amber::runtime::Value shared_right = heap.make_tuple_value({});
  amber::runtime::IntrusivePtr<amber::runtime::TupleValue> shared_left_ptr =
      shared_left.as_tuple();
  amber::runtime::IntrusivePtr<amber::runtime::TupleValue> shared_right_ptr =
      shared_right.as_tuple();
  shared_left_ptr->items.push_back(shared_right);
  shared_right_ptr->items.push_back(shared_left);

  const amber::runtime::RuntimeGcResult rooted_shared = heap.collect_garbage(
      {shared_left}, amber::runtime::RuntimeGcCycle::Shared);
  expect(rooted_shared.marked == 2 && rooted_shared.reclaimed == 0,
         "shared GC should preserve a rooted shared reference cycle");
  expect(shared_left_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "rooted shared cycle left node should stay live");
  expect(shared_right_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "rooted shared cycle right node should stay live");

  shared_left = amber::runtime::Value::null();
  shared_right = amber::runtime::Value::null();
  const amber::runtime::RuntimeGcResult unrooted_shared =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Shared);
  expect(unrooted_shared.reclaimed == 2,
         "shared GC should reclaim shared cycle after roots clear");
  expect(shared_left_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "unrooted shared cycle left node should be tombstoned");
  expect(shared_right_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "unrooted shared cycle right node should be tombstoned");
}

void test_runtime_gc_parallel_smoke() {
  amber::runtime::RuntimeHeap heap;
  std::vector<amber::runtime::Value> shared_roots;
  std::mutex roots_mutex;
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;

  for (std::uint64_t worker = 0; worker < 4; ++worker) {
    threads.emplace_back(
        [&heap, &shared_roots, &roots_mutex, &ready, &go, worker]() {
          amber::runtime::RuntimeWorkerScope scope(30 + worker);
          std::vector<amber::runtime::Value> local_roots;
          for (std::int64_t i = 0; i < 64; ++i) {
            local_roots.push_back(
                heap.make_list_value({amber::runtime::Value::integer(i)}));
          }
          {
            std::lock_guard<std::mutex> lock(roots_mutex);
            shared_roots.insert(shared_roots.end(), local_roots.begin(),
                                local_roots.end());
          }
          ++ready;
          while (!go.load()) {
            std::this_thread::yield();
          }
          std::vector<amber::runtime::Value> snapshot;
          {
            std::lock_guard<std::mutex> lock(roots_mutex);
            snapshot = shared_roots;
          }
          heap.collect_garbage(snapshot, amber::runtime::RuntimeGcCycle::Full);
        });
  }

  while (ready.load() != 4) {
    std::this_thread::yield();
  }
  go = true;
  for (std::thread &thread : threads) {
    thread.join();
  }

  const amber::runtime::RuntimeHeapStats after_threads = heap.stats();
  expect(after_threads.gc_full_cycles >= 4,
         "parallel GC smoke should run full cycles from worker threads");
  expect(after_threads.live_objects == 256,
         "shared roots should keep all worker allocations live");

  shared_roots.clear();
  heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
  expect(heap.stats().live_objects == 0,
         "final full GC should reclaim worker allocations after roots clear");
}

void test_runtime_pin_roots_gc_and_rejects_stale_unpin() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value =
      heap.make_list_value({amber::runtime::Value::integer(1)});
  amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      value.as_list();

  amber::runtime::RuntimePinResult pin = heap.pin(value);
  expect(pin.ok && pin.token.active, "pin should create active token");
  expect(heap.pin_count(value) == 1, "pin count should include active token");
  expect((list->header.flags & amber::runtime::kObjectFlagPinned) != 0U,
         "pin should set object pinned flag");

  value = amber::runtime::Value::null();
  const amber::runtime::RuntimeGcResult pinned_gc =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
  expect(pinned_gc.reclaimed == 0, "active pin should root object for GC");
  expect(list->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "pinned object should remain live after GC");

  amber::runtime::RuntimeUnpinResult first = heap.unpin(&pin.token);
  expect(first.ok && first.unpinned && !pin.token.active,
         "first unpin should deactivate token");
  amber::runtime::RuntimeUnpinResult second = heap.unpin(&pin.token);
  expect(second.ok && !second.unpinned && second.stale,
         "stale/double unpin should be guarded and return false");

  const amber::runtime::RuntimeGcResult after_unpin =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
  expect(after_unpin.reclaimed == 1,
         "unpinned object should be collectable without roots");
  expect(list->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "GC should tombstone object after pin release");
}

void test_runtime_pin_scope_nesting_counts_and_releases() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value = heap.make_list_value({});
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      value.as_list();

  {
    amber::runtime::RuntimePinScope outer(heap, value);
    expect(outer.active(), "outer pin scope should be active");
    expect(heap.pin_count(value) == 1, "outer scope should pin once");
    {
      amber::runtime::RuntimePinScope inner(heap, value);
      expect(inner.active(), "inner pin scope should be active");
      expect(heap.pin_count(value) == 2,
             "nested scope should increment pin count");
      const amber::runtime::RuntimeGcResult gc =
          heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
      expect(gc.reclaimed == 0, "nested active pins should prevent GC reclaim");
    }
    expect(heap.pin_count(value) == 1,
           "inner scope destructor should release one pin");
  }

  expect(heap.pin_count(value) == 0, "outer scope should release final pin");
  expect((list->header.flags & amber::runtime::kObjectFlagPinned) == 0U,
         "final unpin should clear object pinned flag");
}

void test_runtime_pin_scope_releases_during_exception_unwind() {
  amber::runtime::RuntimeHeap heap;
  const amber::runtime::Value value = heap.make_list_value({});
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      value.as_list();

  bool caught = false;
  try {
    amber::runtime::RuntimePinScope scope(heap, value);
    expect(scope.active(), "exception unwind pin scope should be active");
    expect(heap.pin_count(value) == 1,
           "exception unwind pin scope should pin once");
    throw std::runtime_error("unwind pin scope");
  } catch (const std::runtime_error &) {
    caught = true;
  }

  expect(caught, "exception unwind probe should catch thrown exception");
  expect(heap.pin_count(value) == 0,
         "pin scope destructor should release during exception unwind");
  expect((list->header.flags & amber::runtime::kObjectFlagPinned) == 0U,
         "exception unwind should clear object pinned flag");
  const amber::runtime::RuntimeGcResult after_unwind =
      heap.collect_garbage({}, amber::runtime::RuntimeGcCycle::Full);
  expect(after_unwind.reclaimed == 1,
         "object should be collectable after exception-unwind pin release");
}

void test_runtime_pin_opaque_handle_boundary() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::IntrusivePtr<amber::runtime::InstanceValue> instance =
      heap.make_instance_value(2);
  amber::runtime::Value value = amber::runtime::Value::instance(instance);

  amber::runtime::RuntimePinResult pin = heap.pin(value);
  expect(pin.ok, "opaque pin should succeed for ordinary object");
  amber::runtime::RuntimeOpaqueHandleResult handle_result =
      heap.opaque_handle_for(pin.token);
  expect(handle_result.ok && handle_result.handle.active &&
             handle_result.handle.handle_id != 0,
         "opaque handle should be active and identifier based");
  expect(handle_result.handle.allocation_id == instance->header.allocation_id,
         "opaque handle should refer to allocation id, not raw layout");

  amber::runtime::RuntimeOpaqueHandleResult resolved =
      heap.resolve_opaque_handle(handle_result.handle);
  expect(resolved.ok && resolved.value.is_instance_object() &&
             resolved.value.as_instance_object() == instance,
         "opaque handle should resolve through runtime registry");

  amber::runtime::RuntimeOpaqueHandle handle = handle_result.handle;
  amber::runtime::RuntimeOpaqueHandleResult released =
      heap.release_opaque_handle(&handle);
  expect(released.ok && released.released && !handle.active,
         "opaque handle release should deactivate handle");
  amber::runtime::RuntimeOpaqueHandleResult stale =
      heap.resolve_opaque_handle(handle);
  expect(!stale.ok && stale.error_name == "LifetimeError",
         "released opaque handle should not resolve");

  amber::runtime::RuntimeUnpinResult unpin = heap.unpin(&pin.token);
  expect(unpin.unpinned, "opaque pin should unpin cleanly");
}

void test_runtime_pin_buffer_view_mode() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value = heap.make_list_value(
      {amber::runtime::Value::integer(3), amber::runtime::Value::integer(4)});

  amber::runtime::RuntimePinResult opaque = heap.pin(value);
  expect(opaque.ok, "opaque pin should succeed");
  amber::runtime::RuntimeValueBufferViewResult wrong_mode =
      heap.value_buffer_view(opaque.token);
  expect(!wrong_mode.ok && wrong_mode.error_name == "TypeError",
         "buffer view should reject opaque pin token");
  heap.unpin(&opaque.token);

  amber::runtime::RuntimePinResult buffer =
      heap.pin(value, amber::runtime::RuntimePinViewKind::ValueBuffer,
               amber::runtime::RuntimePinPermission::ReadOnly);
  expect(buffer.ok, "buffer pin should succeed for list storage");
  amber::runtime::RuntimeValueBufferViewResult view =
      heap.value_buffer_view(buffer.token);
  expect(view.ok && view.view.active && view.view.size == 2 &&
             view.view.data != nullptr,
         "buffer pin should expose stable value span");
  expect(view.view.data[0].is_integer() && view.view.data[0].as_integer() == 3,
         "buffer view should point at list item storage");
  heap.unpin(&buffer.token);

  amber::runtime::RuntimeValueBufferViewResult after_unpin =
      heap.value_buffer_view(buffer.token);
  expect(!after_unpin.ok && after_unpin.error_name == "LifetimeError",
         "buffer view should reject stale token after unpin");
}

void test_runtime_pin_dealloc_after_pin_violation() {
  using namespace amber::bytecode;

  BcModule module;
  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  BcCode make_list;
  make_list.code_id = 1;
  make_list.kind = CodeKind::Method;
  make_list.reg_count = 2;
  make_list.instructions.push_back({Opcode::LoadK, {{0, false}, {0, false}}});
  make_list.instructions.push_back(
      {Opcode::MakeList, {{1, false}, {0, false}, {1, false}}});
  make_list.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode dealloc;
  dealloc.code_id = 2;
  dealloc.kind = CodeKind::Method;
  dealloc.reg_count = 2;
  dealloc.instructions.push_back(
      {Opcode::ObjDealloc, {{1, false}, {0, false}}});
  dealloc.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {make_list, dealloc};

  amber::runtime::RuntimeWorld world(module);
  amber::runtime::ExecutionResult made = world.execute(1);
  expect(made.ok() && made.value.is_list(),
         "pin dealloc probe should make list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list =
      made.value.as_list();

  amber::runtime::RuntimePinResult pin = world.pin(made.value);
  expect(pin.ok, "world pin should succeed");
  amber::runtime::ExecutionResult blocked = world.execute(2, {made.value});
  expect(!blocked.ok() && blocked.fault.has_value() &&
             blocked.fault->error_name == "PinnedObjectError",
         "OBJ_DEALLOC should reject active pin");
  expect(list->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Live,
         "failed dealloc should not change lifetime state");

  amber::runtime::RuntimeUnpinResult unpin = world.unpin(&pin.token);
  expect(unpin.unpinned, "world unpin should succeed");
  amber::runtime::ExecutionResult released = world.execute(2, {made.value});
  expect(released.ok() && released.value.is_bool() && released.value.as_bool(),
         "OBJ_DEALLOC should succeed after pin release");
  expect(list->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "dealloc after unpin should tombstone list");
  amber::runtime::RuntimePinResult dead_pin = world.pin(made.value);
  expect(!dead_pin.ok && dead_pin.error_name == "UseAfterFreeError",
         "pin should reject deallocated objects");
}

void test_runtime_pin_parallel_race_smoke() {
  amber::runtime::RuntimeHeap heap;
  std::vector<amber::runtime::Value> values;
  for (std::int64_t i = 0; i < 32; ++i) {
    values.push_back(heap.make_list_value({amber::runtime::Value::integer(i)}));
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (std::uint64_t worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&heap, &values, &failures, worker]() {
      amber::runtime::RuntimeWorkerScope scope(80 + worker);
      for (std::size_t i = worker; i < values.size(); i += 4) {
        for (int round = 0; round < 8; ++round) {
          amber::runtime::RuntimePinResult pin = heap.pin(values[i]);
          if (!pin.ok) {
            ++failures;
            continue;
          }
          const amber::runtime::RuntimeGcResult gc = heap.collect_garbage(
              values, amber::runtime::RuntimeGcCycle::Full);
          if (gc.reclaimed != 0) {
            ++failures;
          }
          amber::runtime::RuntimeUnpinResult unpin = heap.unpin(&pin.token);
          if (!unpin.unpinned) {
            ++failures;
          }
        }
      }
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  expect(failures.load() == 0, "parallel pin/unpin smoke should not fail");
  const amber::runtime::RuntimeHeapStats stats = heap.stats();
  expect(stats.pin_tokens_created == 256,
         "parallel smoke should create one token per pin attempt");
  expect(stats.active_pins == 0 && stats.pinned_objects == 0,
         "parallel smoke should release all pins");
}

void test_runtime_native_wait_cancel_poll_uses_active_pin() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value = heap.make_list_value({});
  amber::runtime::RuntimePinResult pin = heap.pin(value);
  expect(pin.ok, "native wait needs active pin");

  amber::runtime::RuntimeNativeWaitResult wait =
      heap.register_native_wait(pin.token);
  expect(wait.ok && wait.handle.active,
         "native wait registration should return active handle");
  amber::runtime::RuntimeNativeWaitHandle handle = wait.handle;
  amber::runtime::RuntimeNativeWaitResult initial_poll =
      heap.poll_native_wait(handle);
  expect(initial_poll.ok && !initial_poll.cancelled,
         "native wait should start without cancellation");

  amber::runtime::RuntimeNativeWaitResult cancel =
      heap.cancel_native_wait(&handle);
  expect(cancel.ok && cancel.cancelled && handle.cancellation_requested,
         "native wait cancel hook should record pending cancellation");
  amber::runtime::RuntimeNativeWaitResult cancelled_poll =
      heap.poll_native_wait(handle);
  expect(cancelled_poll.ok && cancelled_poll.cancelled,
         "native wait poll should observe cancellation");

  amber::runtime::RuntimeNativeWaitResult finish =
      heap.finish_native_wait(&handle);
  expect(finish.ok && finish.finished && !handle.active,
         "native wait finish should deactivate wait handle");
  amber::runtime::RuntimeNativeWaitResult stale_poll =
      heap.poll_native_wait(handle);
  expect(!stale_poll.ok && stale_poll.error_name == "LifetimeError",
         "finished native wait should reject further polls");
  heap.unpin(&pin.token);
}

void test_runtime_awaitable_select_ready_timeout_and_failure() {
  amber::runtime::RuntimeAwaitable awaitable;

  const amber::runtime::RuntimeSelectResult idle =
      amber::runtime::runtime_select(
          {amber::runtime::RuntimeSelectArm::awaitable_arm(awaitable)},
          std::chrono::hours(1), true);
  expect(idle.ok && idle.else_selected,
         "select else should run for a pending awaitable");

  const amber::runtime::RuntimeSelectResult timed_out =
      amber::runtime::runtime_select(
          {amber::runtime::RuntimeSelectArm::awaitable_arm(awaitable)},
          std::chrono::milliseconds(5), false);
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "select should time out for a pending awaitable");

  expect(awaitable.complete(amber::runtime::Value::integer(44)),
         "awaitable completion should transition pending token to ready");
  const amber::runtime::RuntimeSelectResult selected =
      amber::runtime::runtime_select(
          {amber::runtime::RuntimeSelectArm::awaitable_arm(awaitable)},
          std::chrono::milliseconds(20), false);
  expect(selected.ok && selected.selected &&
             selected.kind == amber::runtime::RuntimeSelectArmKind::Await &&
             selected.awaitable_result.ready &&
             selected.awaitable_result.value.as_integer() == 44,
         "select should choose a ready awaitable arm");

  amber::runtime::RuntimeAwaitable failed;
  expect(failed.fail("TypeError", "synthetic awaitable failure"),
         "awaitable fail should publish terminal failure");
  const amber::runtime::RuntimeSelectResult failed_selected =
      amber::runtime::runtime_select(
          {amber::runtime::RuntimeSelectArm::awaitable_arm(failed)},
          std::chrono::milliseconds(20), false);
  expect(!failed_selected.ok && failed_selected.selected &&
             failed_selected.awaitable_result.failed &&
             failed_selected.error_name == "TypeError",
         "select should surface failed awaitables as selected terminal arms");

  const amber::runtime::RuntimeAwaitableStats stats = awaitable.stats();
  expect(stats.completions == 1 && stats.polls >= 3,
         "awaitable stats should count completion and select polls");
}

void test_runtime_awaitable_native_wait_pin_bridge_and_scheduler() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value =
      heap.make_list_value({amber::runtime::Value::integer(1)});
  amber::runtime::RuntimePinResult pin = heap.pin(value);
  expect(pin.ok, "native-backed awaitable needs active pin");

  amber::runtime::RuntimeAwaitable io =
      amber::runtime::RuntimeAwaitable::from_native_wait(heap, pin.token);
  expect(io.state() == amber::runtime::RuntimeAwaitableState::Pending &&
             io.stats().native_backed,
         "native-backed awaitable should start pending with native bridge");

  amber::runtime::RuntimeScheduler scheduler(2);
  std::atomic<bool> waiter_started{false};
  std::atomic<std::int64_t> observed{0};
  const std::uint64_t waiter =
      scheduler.spawn_task([&io, &waiter_started, &observed]() {
        waiter_started = true;
        const amber::runtime::RuntimeAwaitableResult result =
            io.await(std::chrono::milliseconds(500));
        if (result.ok && result.ready && result.value.is_integer()) {
          observed = result.value.as_integer();
        }
      });
  expect(
      wait_for_condition([&waiter_started]() { return waiter_started.load(); },
                         std::chrono::milliseconds(100)),
      "awaitable waiter task should start");
  const std::uint64_t completer =
      scheduler.spawn_sleeping_task(std::chrono::milliseconds(10), [&io]() {
        expect(io.complete(amber::runtime::Value::integer(99)),
               "native-backed awaitable should complete once");
      });

  const amber::runtime::RuntimeTaskJoinResult waiter_join =
      scheduler.join_task(waiter, std::chrono::milliseconds(1000));
  const amber::runtime::RuntimeTaskJoinResult completer_join =
      scheduler.join_task(completer, std::chrono::milliseconds(1000));
  expect(waiter_join.ok,
         "native-backed awaitable waiter task should complete successfully");
  expect(completer_join.ok,
         "native-backed awaitable completer task should complete successfully");
  expect(observed.load() == 99,
         "native-backed awaitable should wake scheduler task with value");

  const amber::runtime::RuntimeAwaitableStats io_stats = io.stats();
  expect(io_stats.completions == 1 && io_stats.native_polls > 0 &&
             io_stats.native_finishes == 1,
         "native-backed completion should poll and finish native wait");
  expect(heap.unpin(&pin.token).unpinned,
         "completed native-backed awaitable should release its pin normally");

  amber::runtime::RuntimePinResult stale_pin = heap.pin(value);
  expect(stale_pin.ok, "stale-pin awaitable needs active pin first");
  amber::runtime::RuntimeAwaitable stale =
      amber::runtime::RuntimeAwaitable::from_native_wait(heap, stale_pin.token);
  expect(heap.unpin(&stale_pin.token).unpinned,
         "test should make native wait pin stale before await");
  const amber::runtime::RuntimeAwaitableResult stale_result =
      stale.await(std::chrono::milliseconds(0));
  expect(!stale_result.ok && stale_result.failed &&
             stale_result.error_name == "LifetimeError",
         "native-backed awaitable should fail when its pin becomes stale");
}

void test_runtime_awaitable_cancellation_finishes_native_wait() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::Value value = heap.make_list_value({});
  amber::runtime::RuntimePinResult pin = heap.pin(value);
  expect(pin.ok, "cancelled native-backed awaitable needs active pin");

  amber::runtime::RuntimeAwaitable awaitable =
      amber::runtime::RuntimeAwaitable::from_native_wait(heap, pin.token);
  amber::runtime::RuntimeScheduler scheduler(2);
  std::atomic<bool> entered{false};
  std::atomic<bool> saw_cancelled_result{false};

  const std::uint64_t task =
      scheduler.spawn_task([&awaitable, &entered, &saw_cancelled_result]() {
        entered = true;
        const amber::runtime::RuntimeAwaitableResult result =
            awaitable.await(std::chrono::hours(1));
        if (result.cancelled && result.error_name == "CancelledError") {
          saw_cancelled_result = true;
        }
        amber::runtime::throw_if_runtime_task_cancelled();
      });

  expect(wait_for_condition([&entered]() { return entered.load(); },
                            std::chrono::milliseconds(100)),
         "cancellable awaitable task should enter await");
  expect(scheduler.cancel_task(task),
         "scheduler should request cancellation for awaitable task");
  const amber::runtime::RuntimeTaskJoinResult joined =
      scheduler.join_task(task, std::chrono::milliseconds(1000));
  expect(joined.cancelled && joined.error_name == "CancelledError" &&
             saw_cancelled_result.load(),
         "awaitable task cancellation should surface and rethrow");

  const amber::runtime::RuntimeAwaitableStats stats = awaitable.stats();
  expect(stats.cancellations == 1 && stats.native_cancellations == 1 &&
             stats.native_finishes == 1,
         "awaitable cancellation should cancel and finish native wait");
  expect(heap.unpin(&pin.token).unpinned,
         "cancelled native-backed awaitable should leave pin releasable");
}

void test_runtime_scheduler_runs_strands_in_parallel() {
  amber::runtime::RuntimeScheduler scheduler(4);
  std::atomic<int> entered{0};
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  std::atomic<bool> release{false};

  for (int index = 0; index < 8; ++index) {
    scheduler.spawn_strand([&entered, &active, &max_active, &release]() {
      expect(amber::runtime::current_runtime_worker_id() != 0,
             "scheduler strand should run inside a worker scope");
      expect(amber::runtime::current_runtime_strand_id() != 0,
             "scheduler strand should expose current strand id");

      const int now = active.fetch_add(1) + 1;
      int observed = max_active.load();
      while (now > observed &&
             !max_active.compare_exchange_weak(observed, now)) {
      }
      entered.fetch_add(1);
      while (!release.load()) {
        std::this_thread::yield();
      }
      active.fetch_sub(1);
    });
  }

  expect(wait_for_condition([&entered]() { return entered.load() >= 4; },
                            std::chrono::milliseconds(1000)),
         "worker pool should start multiple strands");
  expect(max_active.load() >= 2,
         "worker pool should execute strands in parallel");
  release = true;
  expect(scheduler.wait_until_idle(std::chrono::milliseconds(1000)),
         "scheduler should drain runnable strands");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.worker_count == 4, "scheduler should report worker count");
  expect(stats.strands_created == 8 && stats.strands_completed == 8,
         "scheduler stats should count completed strands");
  expect(stats.max_parallel_running >= 2,
         "scheduler stats should observe parallel running strands");
}

void test_runtime_scheduler_timer_queue_wakes_sleeping_strand() {
  amber::runtime::RuntimeScheduler scheduler(2);
  std::atomic<int> ran{0};
  const std::uint64_t strand_id = scheduler.spawn_sleeping_strand(
      std::chrono::milliseconds(25), [&ran]() { ran.fetch_add(1); });

  const std::optional<amber::runtime::RuntimeStrandSnapshot> sleeping =
      scheduler.strand_snapshot(strand_id);
  expect(sleeping.has_value() &&
             sleeping->state == amber::runtime::RuntimeStrandState::Sleeping,
         "delayed strand should begin in sleeping state");
  expect(!scheduler.wait_until_idle(std::chrono::milliseconds(5)),
         "sleeping strand should keep scheduler non-idle before timer fires");
  expect(ran.load() == 0, "sleeping strand should not run before timer wake");

  expect(scheduler.wait_until_idle(std::chrono::milliseconds(1000)),
         "timer queue should wake delayed strand");
  expect(ran.load() == 1, "timer wake should run sleeping strand once");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.timer_wakes == 1, "scheduler should count timer wake");
  expect(stats.local_queue_enqueues == 1,
         "timer wake should enter a worker-local run queue");
}

void test_runtime_scheduler_explicit_wake_coalesces_sleeping_strand() {
  amber::runtime::RuntimeScheduler scheduler(2);
  std::atomic<int> ran{0};
  std::atomic<std::uint64_t> observed_strand{0};
  std::atomic<bool> release{false};

  const std::uint64_t strand_id = scheduler.spawn_sleeping_strand(
      std::chrono::hours(1), [&ran, &observed_strand, &release]() {
        observed_strand = amber::runtime::current_runtime_strand_id();
        ran.fetch_add(1);
        while (!release.load()) {
          std::this_thread::yield();
        }
      });

  expect(scheduler.wake_strand(strand_id),
         "first explicit wake should make sleeping strand runnable");
  expect(!scheduler.wake_strand(strand_id),
         "duplicate wake should be coalesced while strand is queued/running");
  release = true;
  expect(scheduler.wait_until_idle(std::chrono::milliseconds(1000)),
         "explicit wake should drain sleeping strand without waiting timer");
  expect(ran.load() == 1, "coalesced wakes should run strand once");
  expect(observed_strand.load() == strand_id,
         "woken strand should preserve current strand id");

  const std::optional<amber::runtime::RuntimeStrandSnapshot> finished =
      scheduler.strand_snapshot(strand_id);
  expect(finished.has_value() &&
             finished->state == amber::runtime::RuntimeStrandState::Finished,
         "woken strand should finish");
  expect(finished->explicit_wakes == 1,
         "strand snapshot should count one accepted explicit wake");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.explicit_wakes == 1 && stats.coalesced_wakes == 1,
         "scheduler should count accepted and coalesced wakes");
  expect(stats.strands_completed == 1,
         "explicit wake strand should complete exactly once");
  expect(stats.timer_queue_depth == 0 && stats.sleeping_strands == 0,
         "stale timer entry should not keep a woken strand logically sleeping");
}

void test_runtime_task_join_rethrows_failure() {
  amber::runtime::RuntimeScheduler scheduler(2);
  const std::uint64_t task_id = scheduler.spawn_task([]() {
    throw amber::runtime::RuntimeTaskFailure("BoomError", "child failed");
  });

  const amber::runtime::RuntimeTaskJoinResult join =
      scheduler.join_task(task_id, std::chrono::milliseconds(1000));
  expect(!join.ok && join.joined &&
             join.state == amber::runtime::RuntimeStrandState::Failed,
         "join should observe failed task state");
  expect(join.error_name == "BoomError" && join.message == "child failed",
         "join should rethrow task failure metadata");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.tasks_created == 1 && stats.tasks_failed == 1,
         "task stats should count failed task");
}

void test_runtime_task_join_timeout_does_not_cancel_task() {
  amber::runtime::RuntimeScheduler scheduler(2);
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};

  const std::uint64_t task_id = scheduler.spawn_task([&entered, &release]() {
    entered = true;
    while (!release.load()) {
      std::this_thread::yield();
    }
  });

  expect(wait_for_condition([&entered]() { return entered.load(); },
                            std::chrono::milliseconds(1000)),
         "timeout probe task should start");
  const amber::runtime::RuntimeTaskJoinResult timed_out =
      scheduler.join_task(task_id, std::chrono::milliseconds(10));
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "timed join should return TimeoutError");

  const std::optional<amber::runtime::RuntimeTaskSnapshot> snapshot =
      scheduler.task_snapshot(task_id);
  expect(snapshot.has_value() && !snapshot->cancellation_requested,
         "join timeout should not request task cancellation");

  release = true;
  const amber::runtime::RuntimeTaskJoinResult joined =
      scheduler.join_task(task_id, std::chrono::milliseconds(1000));
  expect(joined.ok && joined.state == amber::runtime::RuntimeStrandState::Done,
         "task should still finish after join timeout");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.task_join_timeouts == 1 && stats.tasks_completed == 1,
         "task stats should count timeout and later completion");
}

void test_runtime_task_cancel_is_cooperative_safepoint() {
  amber::runtime::RuntimeScheduler scheduler(2);
  std::atomic<bool> entered{false};
  std::atomic<bool> observed_cancel{false};

  const std::uint64_t task_id =
      scheduler.spawn_task([&entered, &observed_cancel]() {
        entered = true;
        while (!amber::runtime::current_runtime_task_cancel_requested()) {
          std::this_thread::yield();
        }
        observed_cancel = true;
        amber::runtime::throw_if_runtime_task_cancelled();
      });

  expect(wait_for_condition([&entered]() { return entered.load(); },
                            std::chrono::milliseconds(1000)),
         "cancellable task should start");
  expect(scheduler.cancel_task(task_id),
         "cancel_task should request cancellation");

  const amber::runtime::RuntimeTaskJoinResult join =
      scheduler.join_task(task_id, std::chrono::milliseconds(1000));
  expect(!join.ok && join.cancelled && join.error_name == "CancelledError",
         "cancelled task should join with CancelledError");
  expect(observed_cancel.load(), "task should observe cancellation flag");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.task_cancellation_requests == 1 && stats.tasks_cancelled == 1,
         "task stats should count cooperative cancellation");
}

void test_runtime_structured_task_scope_propagates_first_failure() {
  amber::runtime::RuntimeScheduler scheduler(3);
  std::atomic<bool> sibling_started{false};
  std::atomic<bool> sibling_cancelled{false};
  std::atomic<bool> allow_failure{false};

  const std::uint64_t parent_id = scheduler.spawn_task(
      [&scheduler, &sibling_started, &sibling_cancelled, &allow_failure]() {
        scheduler.spawn_task([&sibling_started, &sibling_cancelled]() {
          sibling_started = true;
          while (!amber::runtime::current_runtime_task_cancel_requested()) {
            std::this_thread::yield();
          }
          sibling_cancelled = true;
          amber::runtime::throw_if_runtime_task_cancelled();
        });
        scheduler.spawn_task([&allow_failure]() {
          while (!allow_failure.load()) {
            std::this_thread::yield();
          }
          throw amber::runtime::RuntimeTaskFailure("ChildBoom",
                                                   "first child failed");
        });
      });

  expect(
      wait_for_condition(
          [&scheduler, parent_id, &sibling_started]() {
            const std::optional<amber::runtime::RuntimeTaskSnapshot> snapshot =
                scheduler.task_snapshot(parent_id);
            return snapshot.has_value() &&
                   snapshot->state ==
                       amber::runtime::RuntimeStrandState::Waiting &&
                   snapshot->active_children == 2 && sibling_started.load();
          },
          std::chrono::milliseconds(1000)),
      "parent should wait for structured children at scope exit");
  expect(sibling_started.load(), "sibling should be running before failure");

  allow_failure = true;
  const amber::runtime::RuntimeTaskJoinResult join =
      scheduler.join_task(parent_id, std::chrono::milliseconds(1000));
  expect(!join.ok && join.joined &&
             join.state == amber::runtime::RuntimeStrandState::Failed,
         "parent join should fail after first child failure");
  expect(join.error_name == "ChildBoom" && join.message == "first child failed",
         "parent join should report first child failure");
  expect(sibling_cancelled.load(),
         "first child failure should cancel running sibling");

  const std::optional<amber::runtime::RuntimeTaskSnapshot> snapshot =
      scheduler.task_snapshot(parent_id);
  expect(snapshot.has_value() &&
             snapshot->state == amber::runtime::RuntimeStrandState::Failed &&
             snapshot->total_children == 2 && snapshot->active_children == 0,
         "failed parent should retain structured child snapshot");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.structured_child_tasks == 2 &&
             stats.first_failure_cancellations == 1,
         "structured stats should count child links and first-failure cancel");
  expect(stats.tasks_failed == 2 && stats.tasks_cancelled == 1,
         "structured failure should fail child and parent, and cancel sibling");
}

void test_runtime_channel_rendezvous_fifo_close() {
  amber::runtime::RuntimeChannel channel(0);
  amber::runtime::RuntimeScheduler scheduler(4);
  std::atomic<int> senders_released{0};
  std::mutex received_mutex;
  std::vector<std::int64_t> received;

  for (int index = 1; index <= 3; ++index) {
    scheduler.spawn_task([&channel, &senders_released, index]() {
      const amber::runtime::RuntimeChannelResult send =
          channel.send(amber::runtime::Value::integer(index),
                       std::chrono::milliseconds(1000));
      expect(send.ok && send.sent, "rendezvous channel send should complete");
      senders_released.fetch_add(1);
    });
    expect(wait_for_condition(
               [&channel, index]() {
                 return channel.stats().pending_senders ==
                        static_cast<std::uint64_t>(index);
               },
               std::chrono::milliseconds(1000)),
           "rendezvous sender should enter FIFO wait queue");
  }

  expect(senders_released.load() == 0,
         "rendezvous sends should not complete before recv");

  scheduler.spawn_task([&channel, &received, &received_mutex]() {
    for (int expected = 1; expected <= 3; ++expected) {
      const amber::runtime::RuntimeChannelResult recv =
          channel.recv(std::chrono::milliseconds(1000));
      expect(recv.ok && recv.received && recv.value.is_integer(),
             "rendezvous channel recv should return sent value");
      std::lock_guard<std::mutex> lock(received_mutex);
      received.push_back(recv.value.as_integer());
      expect(recv.value.as_integer() == expected,
             "rendezvous channel should preserve FIFO send order");
    }
    expect(channel.close(), "channel close should succeed once");
    const amber::runtime::RuntimeChannelResult closed =
        channel.recv(std::chrono::milliseconds(10));
    expect(!closed.ok && closed.closed &&
               closed.error_name == "ChannelClosedError",
           "closed empty channel recv should report ChannelClosedError");
  });

  expect(scheduler.wait_until_idle(std::chrono::milliseconds(1000)),
         "rendezvous channel tasks should drain");
  expect(senders_released.load() == 3,
         "all rendezvous senders should complete after receives");
  {
    std::lock_guard<std::mutex> lock(received_mutex);
    expect(received.size() == 3 && received[0] == 1 && received[1] == 2 &&
               received[2] == 3,
           "rendezvous channel should receive all values in order");
  }

  const amber::runtime::RuntimeChannelStats stats = channel.stats();
  expect(stats.sends == 3 && stats.receives == 3 && stats.closes == 1,
         "rendezvous channel stats should count sends, receives, and close");
}

void test_runtime_channel_buffered_close_and_shareability_gate() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::RuntimeChannel channel(2);

  const amber::runtime::RuntimeChannelResult first =
      channel.send(amber::runtime::Value::integer(10));
  const amber::runtime::RuntimeChannelResult second =
      channel.send(amber::runtime::Value::integer(11));
  expect(first.ok && second.ok, "buffered channel should accept capacity");

  const amber::runtime::Value confined = heap.make_list_value({});
  expect(!amber::runtime::runtime_value_is_shareable(confined),
         "mutable list should not be shareable");
  const amber::runtime::RuntimeChannelResult rejected =
      channel.send(confined, std::chrono::milliseconds(0));
  expect(!rejected.ok && rejected.error_name == "IsolationError",
         "channel send should reject confined payloads");

  const amber::runtime::Value transitively_confined =
      heap.make_list_value({confined}, true);
  expect(!amber::runtime::runtime_value_is_shareable(transitively_confined),
         "frozen collection with confined payload should not be shareable");
  const amber::runtime::RuntimeChannelResult nested_rejected =
      channel.send(transitively_confined, std::chrono::milliseconds(0));
  expect(!nested_rejected.ok && nested_rejected.error_name == "IsolationError",
         "channel send should reject transitively confined payloads");

  const amber::runtime::RuntimeChannelResult recv_first = channel.recv();
  const amber::runtime::RuntimeChannelResult recv_second = channel.recv();
  expect(recv_first.ok && recv_first.value.as_integer() == 10,
         "buffered channel should receive first queued value");
  expect(recv_second.ok && recv_second.value.as_integer() == 11,
         "buffered channel should receive second queued value");

  const amber::runtime::RuntimeChannelResult timed_out =
      channel.recv(std::chrono::milliseconds(5));
  expect(!timed_out.ok && timed_out.timed_out &&
             timed_out.error_name == "TimeoutError",
         "empty open channel recv should support timeout");

  const amber::runtime::RuntimeChannelResult queued_after_timeout =
      channel.send(amber::runtime::Value::integer(12));
  expect(queued_after_timeout.ok, "buffered channel should accept later send");
  expect(channel.close(), "buffered channel close should succeed");
  const amber::runtime::RuntimeChannelResult send_after_close =
      channel.send(amber::runtime::Value::integer(13));
  expect(!send_after_close.ok && send_after_close.closed &&
             send_after_close.error_name == "ChannelClosedError",
         "send into closed channel should fail");
  const amber::runtime::RuntimeChannelResult recv_buffered_after_close =
      channel.recv();
  expect(recv_buffered_after_close.ok &&
             recv_buffered_after_close.value.as_integer() == 12,
         "closed buffered channel should drain queued values first");
  const amber::runtime::RuntimeChannelResult closed_empty = channel.recv();
  expect(!closed_empty.ok && closed_empty.closed &&
             closed_empty.error_name == "ChannelClosedError",
         "closed empty buffered channel should fail recv");

  const amber::runtime::RuntimeChannelStats stats = channel.stats();
  expect(stats.sends == 3 && stats.receives == 3 &&
             stats.receive_timeouts == 1 && stats.isolation_rejections == 2,
         "buffered channel stats should count success, timeout, and isolation");
}

void test_runtime_move_slot_channel_transfer_and_moved_guard() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::RuntimeChannel plain_channel(1);

  amber::runtime::Value list;
  {
    amber::runtime::RuntimeStrandScope sender_scope(41);
    list = heap.make_list_value({amber::runtime::Value::integer(7)}, false);
  }

  const amber::runtime::RuntimeChannelResult plain_send =
      plain_channel.send(list, std::chrono::milliseconds(0));
  expect(!plain_send.ok && plain_send.error_name == "IsolationError",
         "plain channel send should reject confined mutable payload");

  amber::runtime::RuntimeMoveSlot slot(list);
  amber::runtime::RuntimeChannel moved_channel(1);
  {
    amber::runtime::RuntimeStrandScope sender_scope(41);
    const amber::runtime::RuntimeChannelResult moved_send =
        moved_channel.send(slot, std::chrono::milliseconds(10));
    expect(moved_send.ok && moved_send.sent,
           "moved channel send should accept confined payload");
  }
  expect(slot.moved(), "successful moved send should mark slot moved");

  const amber::runtime::RuntimeMoveResult moved_read = slot.read();
  expect(!moved_read.ok && moved_read.error_name == "MovedValueError",
         "reading moved-from slot should fail");
  expect(list.as_list()->header.owner.kind ==
             amber::runtime::OwnerTokenKind::Sync,
         "moved payload should be in transit before recv");

  {
    amber::runtime::RuntimeStrandScope receiver_scope(42);
    const amber::runtime::RuntimeChannelResult moved_recv =
        moved_channel.recv(std::chrono::milliseconds(10));
    expect(moved_recv.ok && moved_recv.received && moved_recv.value.is_list(),
           "moved channel recv should return payload");
    expect(moved_recv.value.as_list()->header.owner.kind ==
                   amber::runtime::OwnerTokenKind::Confined &&
               moved_recv.value.as_list()->header.owner.strand_id == 42,
           "recv should adopt moved payload to receiver strand");
  }

  const amber::runtime::Value frozen = heap.make_list_value({}, true);
  amber::runtime::RuntimeMoveSlot frozen_slot(frozen);
  const amber::runtime::RuntimeChannelResult rejected_move =
      moved_channel.send(frozen_slot, std::chrono::milliseconds(0));
  expect(!rejected_move.ok && rejected_move.error_name == "MoveError",
         "move should reject already-shareable payloads");
  expect(frozen_slot.read().ok,
         "failed move reservation should leave source slot readable");
}

void test_runtime_select_rotates_ready_arms_and_handles_else_timeout() {
  amber::runtime::RuntimeChannel first(1);
  amber::runtime::RuntimeChannel second(1);
  int first_selected = 0;
  int second_selected = 0;

  for (int iteration = 0; iteration < 8; ++iteration) {
    expect(first.send(amber::runtime::Value::integer(iteration)).ok,
           "first select channel should accept buffered value");
    expect(second.send(amber::runtime::Value::integer(100 + iteration)).ok,
           "second select channel should accept buffered value");

    const amber::runtime::RuntimeSelectResult selected =
        amber::runtime::runtime_select(
            {amber::runtime::RuntimeSelectArm::recv(first),
             amber::runtime::RuntimeSelectArm::recv(second)},
            std::chrono::milliseconds(20));
    expect(selected.ok && selected.selected &&
               selected.kind == amber::runtime::RuntimeSelectArmKind::Recv,
           "select should choose a ready recv arm");
    if (selected.arm_index == 0) {
      ++first_selected;
      expect(second.recv().ok, "unselected second channel should drain");
    } else {
      ++second_selected;
      expect(first.recv().ok, "unselected first channel should drain");
    }
  }
  expect(first_selected > 0 && second_selected > 0,
         "select should rotate among ready arms instead of fixed left bias");

  amber::runtime::RuntimeChannel empty(0);
  const amber::runtime::RuntimeSelectResult else_result =
      amber::runtime::runtime_select(
          {amber::runtime::RuntimeSelectArm::recv(empty)},
          std::chrono::hours(1), true);
  expect(else_result.ok && else_result.else_selected,
         "select else should run immediately when no arm is ready");

  const amber::runtime::RuntimeSelectResult timeout_result =
      amber::runtime::runtime_select(
          {amber::runtime::RuntimeSelectArm::recv(empty)},
          std::chrono::milliseconds(5), false);
  expect(!timeout_result.ok && timeout_result.timed_out &&
             timeout_result.error_name == "TimeoutError",
         "select without else should support bounded timeout");
}

void test_runtime_select_send_move_arm_commits_only_when_ready() {
  amber::runtime::RuntimeHeap heap;
  amber::runtime::RuntimeChannel rendezvous(0);
  amber::runtime::RuntimeChannel buffered(1);
  amber::runtime::Value packet;
  {
    amber::runtime::RuntimeStrandScope sender_scope(71);
    packet = heap.make_list_value({amber::runtime::Value::integer(71)}, false);
  }

  amber::runtime::RuntimeMoveSlot slot(packet);
  {
    amber::runtime::RuntimeStrandScope sender_scope(71);
    const amber::runtime::RuntimeSelectResult idle =
        amber::runtime::runtime_select(
            {amber::runtime::RuntimeSelectArm::send_moved(rendezvous, slot)},
            std::chrono::hours(1), true);
    expect(
        idle.ok && idle.else_selected,
        "select send arm should not commit move when rendezvous is not ready");
    expect(slot.read().ok,
           "unselected moved send arm should leave slot readable");

    const amber::runtime::RuntimeSelectResult sent =
        amber::runtime::runtime_select(
            {amber::runtime::RuntimeSelectArm::send_moved(buffered, slot)},
            std::chrono::milliseconds(20), false);
    expect(sent.ok && sent.selected && sent.channel_result.sent,
           "ready select send arm should commit moved payload");
  }
  expect(!slot.read().ok && slot.moved(),
         "selected moved send arm should mark slot moved-from");

  {
    amber::runtime::RuntimeStrandScope receiver_scope(72);
    const amber::runtime::RuntimeChannelResult recv = buffered.recv();
    expect(recv.ok && recv.value.as_list()->header.owner.strand_id == 72,
           "select moved send payload should be adopted by receiver");
  }
}

void test_runtime_supervisor_one_for_one_keeps_sibling_running() {
  amber::runtime::RuntimeScheduler scheduler(3);
  std::atomic<bool> sibling_started{false};
  std::atomic<bool> sibling_cancelled{false};
  std::atomic<bool> release_sibling{false};
  std::atomic<bool> allow_failure{false};

  amber::runtime::RuntimeTaskOptions options;
  options.policy = amber::runtime::RuntimeSupervisorPolicy::OneForOne;
  const std::uint64_t parent_id = scheduler.spawn_task(
      options, [&scheduler, &sibling_started, &sibling_cancelled,
                &release_sibling, &allow_failure]() {
        scheduler.spawn_task(
            [&sibling_started, &sibling_cancelled, &release_sibling]() {
              sibling_started = true;
              while (!release_sibling.load() &&
                     !amber::runtime::current_runtime_task_cancel_requested()) {
                std::this_thread::yield();
              }
              if (amber::runtime::current_runtime_task_cancel_requested()) {
                sibling_cancelled = true;
                amber::runtime::throw_if_runtime_task_cancelled();
              }
            });
        scheduler.spawn_task([&allow_failure]() {
          while (!allow_failure.load()) {
            std::this_thread::yield();
          }
          throw amber::runtime::RuntimeTaskFailure("ChildBoom",
                                                   "one-for-one child failed");
        });
      });

  expect(wait_for_condition(
             [&scheduler, parent_id, &sibling_started]() {
               const auto snapshot = scheduler.task_snapshot(parent_id);
               return snapshot.has_value() && snapshot->active_children == 2 &&
                      sibling_started.load();
             },
             std::chrono::milliseconds(1000)),
         "one-for-one parent should wait for both children");

  allow_failure = true;
  expect(wait_for_condition(
             [&scheduler, parent_id]() {
               const auto snapshot = scheduler.task_snapshot(parent_id);
               return snapshot.has_value() && snapshot->active_children == 1;
             },
             std::chrono::milliseconds(1000)),
         "one-for-one child failure should leave sibling active");
  expect(!sibling_cancelled.load(),
         "one-for-one policy should not cancel unrelated sibling");

  const amber::runtime::RuntimeTaskJoinResult timed =
      scheduler.join_task(parent_id, std::chrono::milliseconds(10));
  expect(!timed.ok && timed.timed_out,
         "one-for-one parent should keep waiting for live sibling");

  release_sibling = true;
  const amber::runtime::RuntimeTaskJoinResult joined =
      scheduler.join_task(parent_id, std::chrono::milliseconds(1000));
  expect(!joined.ok && joined.joined && joined.error_name == "ChildBoom",
         "one-for-one parent should report failed child after siblings drain");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.supervisor_one_for_one_failures == 1 &&
             stats.first_failure_cancellations == 0,
         "one-for-one stats should record non-cancelling child failure");
}

void test_runtime_supervisor_one_for_all_cancels_all_siblings() {
  amber::runtime::RuntimeScheduler scheduler(4);
  std::atomic<int> sibling_cancellations{0};
  std::atomic<bool> allow_failure{false};

  amber::runtime::RuntimeTaskOptions options;
  options.policy = amber::runtime::RuntimeSupervisorPolicy::OneForAll;
  const std::uint64_t parent_id = scheduler.spawn_task(
      options, [&scheduler, &sibling_cancellations, &allow_failure]() {
        for (int index = 0; index < 2; ++index) {
          scheduler.spawn_task([&sibling_cancellations]() {
            while (!amber::runtime::current_runtime_task_cancel_requested()) {
              std::this_thread::yield();
            }
            sibling_cancellations.fetch_add(1);
            amber::runtime::throw_if_runtime_task_cancelled();
          });
        }
        scheduler.spawn_task([&allow_failure]() {
          while (!allow_failure.load()) {
            std::this_thread::yield();
          }
          throw amber::runtime::RuntimeTaskFailure("AllBoom",
                                                   "one-for-all failed");
        });
      });

  expect(wait_for_condition(
             [&scheduler, parent_id]() {
               const auto snapshot = scheduler.task_snapshot(parent_id);
               return snapshot.has_value() && snapshot->active_children == 3;
             },
             std::chrono::milliseconds(1000)),
         "one-for-all parent should start all children");
  allow_failure = true;

  const amber::runtime::RuntimeTaskJoinResult joined =
      scheduler.join_task(parent_id, std::chrono::milliseconds(1000));
  expect(!joined.ok && joined.error_name == "AllBoom",
         "one-for-all parent should fail with child error");
  expect(sibling_cancellations.load() == 2,
         "one-for-all should cancel all running siblings");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.supervisor_one_for_all_cancellations == 1,
         "one-for-all stats should record policy cancellation");
}

void test_runtime_supervisor_rest_for_one_cancels_later_siblings_only() {
  amber::runtime::RuntimeScheduler scheduler(4);
  std::atomic<bool> earlier_started{false};
  std::atomic<bool> later_started{false};
  std::atomic<bool> earlier_cancelled{false};
  std::atomic<bool> later_cancelled{false};
  std::atomic<bool> release_earlier{false};
  std::atomic<bool> allow_failure{false};

  amber::runtime::RuntimeTaskOptions options;
  options.policy = amber::runtime::RuntimeSupervisorPolicy::RestForOne;
  const std::uint64_t parent_id = scheduler.spawn_task(
      options,
      [&scheduler, &earlier_started, &earlier_cancelled, &later_started,
       &later_cancelled, &release_earlier, &allow_failure]() {
        scheduler.spawn_task(
            [&earlier_started, &earlier_cancelled, &release_earlier]() {
              earlier_started = true;
              while (!release_earlier.load() &&
                     !amber::runtime::current_runtime_task_cancel_requested()) {
                std::this_thread::yield();
              }
              if (amber::runtime::current_runtime_task_cancel_requested()) {
                earlier_cancelled = true;
                amber::runtime::throw_if_runtime_task_cancelled();
              }
            });
        scheduler.spawn_task([&allow_failure]() {
          while (!allow_failure.load()) {
            std::this_thread::yield();
          }
          throw amber::runtime::RuntimeTaskFailure("RestBoom",
                                                   "middle child failed");
        });
        scheduler.spawn_task([&later_started, &later_cancelled]() {
          later_started = true;
          while (!amber::runtime::current_runtime_task_cancel_requested()) {
            std::this_thread::yield();
          }
          later_cancelled = true;
          amber::runtime::throw_if_runtime_task_cancelled();
        });
      });

  expect(wait_for_condition(
             [&scheduler, parent_id, &earlier_started]() {
               const auto snapshot = scheduler.task_snapshot(parent_id);
               return snapshot.has_value() && snapshot->active_children == 3 &&
                      earlier_started.load();
             },
             std::chrono::milliseconds(1000)),
         "rest-for-one parent should start ordered children");
  expect(wait_for_condition([&later_started]() { return later_started.load(); },
                            std::chrono::milliseconds(1000)),
         "rest-for-one later sibling should start before failure is released");
  allow_failure = true;
  expect(wait_for_condition(
             [&later_cancelled]() { return later_cancelled.load(); },
             std::chrono::milliseconds(1000)),
         "rest-for-one should cancel later sibling");
  expect(!earlier_cancelled.load(),
         "rest-for-one should leave earlier sibling running");

  release_earlier = true;
  const amber::runtime::RuntimeTaskJoinResult joined =
      scheduler.join_task(parent_id, std::chrono::milliseconds(1000));
  expect(!joined.ok && joined.error_name == "RestBoom",
         "rest-for-one parent should fail with first child error");

  const amber::runtime::RuntimeSchedulerStats stats = scheduler.stats();
  expect(stats.supervisor_rest_for_one_cancellations == 1,
         "rest-for-one stats should count later sibling cancellation");
}

void test_runtime_mutex_non_reentrant_and_contention() {
  amber::runtime::RuntimeMutex reentrant_probe;
  const amber::runtime::RuntimeMutexResult first_lock =
      reentrant_probe.lock(std::chrono::milliseconds(10));
  expect(first_lock.ok && first_lock.locked, "mutex first lock should succeed");
  const amber::runtime::RuntimeMutexResult second_lock =
      reentrant_probe.lock(std::chrono::milliseconds(10));
  expect(!second_lock.ok && second_lock.error_name == "DeadlockError",
         "mutex should reject reentrant lock by same owner");
  const amber::runtime::RuntimeMutexResult first_unlock =
      reentrant_probe.unlock();
  expect(first_unlock.ok && first_unlock.unlocked,
         "mutex unlock should release owner");

  amber::runtime::RuntimeScheduler scheduler(4);
  amber::runtime::RuntimeMutex mutex;
  int counter = 0;
  constexpr int kTasks = 8;
  constexpr int kIterations = 200;

  for (int task = 0; task < kTasks; ++task) {
    scheduler.spawn_task([&mutex, &counter]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const amber::runtime::RuntimeMutexResult lock =
            mutex.lock(std::chrono::milliseconds(1000));
        expect(lock.ok && lock.locked,
               "contended mutex lock should eventually succeed");
        const int next = counter + 1;
        if (iteration % 5 == 0) {
          std::this_thread::yield();
        }
        counter = next;
        const amber::runtime::RuntimeMutexResult unlock = mutex.unlock();
        expect(unlock.ok && unlock.unlocked,
               "contended mutex unlock should succeed");
      }
    });
  }

  expect(scheduler.wait_until_idle(std::chrono::milliseconds(3000)),
         "mutex contention tasks should drain");
  expect(counter == kTasks * kIterations,
         "mutex should protect contended counter updates");
  const amber::runtime::RuntimeMutexStats stats = mutex.stats();
  expect(stats.locks == kTasks * kIterations &&
             stats.unlocks == kTasks * kIterations && !stats.locked,
         "mutex stats should count balanced lock/unlock operations");
}

void test_runtime_atomic_seq_cst_compare_and_set_visibility() {
  amber::runtime::RuntimeAtomic probe(0);
  expect(probe.get() == 0, "atomic get should read initial value");
  expect(probe.compare_and_set(0, 1),
         "atomic compare_and_set should update matching value");
  expect(!probe.compare_and_set(0, 2),
         "atomic compare_and_set should reject stale expected value");
  probe.set(3);
  expect(probe.get() == 3, "atomic set should publish new value");

  amber::runtime::RuntimeScheduler scheduler(4);
  amber::runtime::RuntimeAtomic counter(0);
  constexpr int kTasks = 8;
  constexpr int kIterations = 500;
  for (int task = 0; task < kTasks; ++task) {
    scheduler.spawn_task([&counter]() {
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        while (true) {
          const std::int64_t current = counter.get();
          if (counter.compare_and_set(current, current + 1)) {
            break;
          }
          std::this_thread::yield();
        }
      }
    });
  }

  expect(scheduler.wait_until_idle(std::chrono::milliseconds(3000)),
         "atomic CAS increment tasks should drain");
  expect(counter.get() == kTasks * kIterations,
         "atomic compare_and_set should preserve all contended increments");
}

void test_runtime_world_heap_tracks_vm_allocations() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"k"};

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 8;
  code.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  code.instructions.push_back(
      {Opcode::MakeList, {{2, false}, {1, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::MakeTuple, {{3, false}, {1, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::MakeMap, {{4, false}, {1, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::MakeClosure,
       {{5, false}, {2, false}, {1, false}, {0, false}, {1, false}}});
  code.instructions.push_back(
      {Opcode::Call,
       {{6, false}, {0, false}, {0, false}, {0, false}, {-1, true}}});
  code.instructions.push_back({Opcode::Return, {{2, false}}});

  BcCode closure_code;
  closure_code.code_id = 2;
  closure_code.kind = CodeKind::Block;
  closure_code.reg_count = 1;
  closure_code.instructions.push_back({Opcode::LoadNull, {{0, false}}});
  closure_code.instructions.push_back({Opcode::Return, {{0, false}}});

  module.classes.push_back(BcClass{});
  module.code_objects = {code, closure_code};

  amber::runtime::RuntimeWorld world(module);
  {
    amber::runtime::RuntimeWorkerScope worker(11);
    const amber::runtime::ExecutionResult exec =
        world.execute(1, {amber::runtime::Value::class_object(0)});
    expect(exec.ok(), "VM allocation probe should execute");
    expect(exec.value.is_list(), "VM allocation probe should return list");
    expect(exec.value.as_list()->header.arena_worker_id == 11,
           "VM list should be allocated in current worker arena");

    const amber::runtime::RuntimeHeapStats stats = world.heap_stats();
    const amber::runtime::RuntimeArenaStats *arena = arena_stats_for(stats, 11);
    expect(arena != nullptr && arena->allocations == 5,
           "VM should allocate list/tuple/map/closure/instance through heap");
    expect(stats.instance_allocations == 1 && stats.array_allocations == 2 &&
               stats.map_allocations == 1 && stats.closure_allocations == 1,
           "world heap stats should include all VM heap object families");
  }

  expect(world.heap_stats().live_objects == 0,
         "VM allocation probe should release returned value after scope");
}

void test_runtime_lifecycle_destroy_opcode_is_idempotent() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "value", "destroy!", "mass"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant answer;
  answer.kind = ConstantKind::Integer;
  answer.int_value = 42;
  module.const_pool.push_back(answer);

  Constant destroyed_marker;
  destroyed_marker.kind = ConstantKind::Integer;
  destroyed_marker.int_value = 99;
  module.const_pool.push_back(destroyed_marker);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 2;
  module.classes.push_back(box);

  BcMethod value_method;
  value_method.selector_sym_id = 1;
  value_method.owner_dispatch_ref = 0;
  value_method.signature_blob_id = 0;
  value_method.entry_code_id = 3;
  value_method.flags = 1;
  module.methods.push_back(value_method);

  BcMethod destroy_method;
  destroy_method.selector_sym_id = 2;
  destroy_method.owner_dispatch_ref = 0;
  destroy_method.signature_blob_id = 0;
  destroy_method.entry_code_id = 4;
  destroy_method.flags = 1;
  module.methods.push_back(destroy_method);

  BcCode destroy;
  destroy.code_id = 1;
  destroy.kind = CodeKind::Method;
  destroy.reg_count = 2;
  destroy.instructions.push_back(
      {Opcode::ObjDestroy, {{1, false}, {0, false}}});
  destroy.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode send_after_destroy;
  send_after_destroy.code_id = 2;
  send_after_destroy.kind = CodeKind::Method;
  send_after_destroy.reg_count = 2;
  send_after_destroy.instructions.push_back({Opcode::Send,
                                             {{1, false},
                                              {0, false},
                                              {1, false},
                                              {0, false},
                                              {0, false},
                                              {-1, true},
                                              {0, false}}});
  send_after_destroy.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 3;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode destroy_body;
  destroy_body.code_id = 4;
  destroy_body.kind = CodeKind::Method;
  destroy_body.reg_count = 3;
  destroy_body.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  destroy_body.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {2, false}}});
  destroy_body.instructions.push_back(
      {Opcode::StoreIvar, {{0, false}, {3, false}, {1, false}, {0, false}}});
  destroy_body.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {destroy, send_after_destroy, body, destroy_body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;

  const amber::runtime::ExecutionResult first = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(first.ok(), "OBJ_DESTROY first execution should succeed");
  expect(first.value.is_bool() && first.value.as_bool(),
         "OBJ_DESTROY should return true for the first destroy");
  expect(instance->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Destroyed,
         "OBJ_DESTROY should transition object to destroyed state");
  expect((instance->header.flags & amber::runtime::kObjectFlagDestroyed) != 0U,
         "OBJ_DESTROY should set destroyed flag");
  expect(instance->ivars.find("mass") != instance->ivars.end() &&
             instance->ivars["mass"].is_integer() &&
             instance->ivars["mass"].as_integer() == 99,
         "OBJ_DESTROY should run class-local destroy! body before tombstoning");

  const amber::runtime::ExecutionResult second = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(second.ok(), "OBJ_DESTROY second execution should succeed");
  expect(second.value.is_bool() && !second.value.as_bool(),
         "OBJ_DESTROY should return false after object is already destroyed");

  const amber::runtime::ExecutionResult send = amber::runtime::execute_code(
      module, 2, {amber::runtime::Value::instance(instance)});
  expect(!send.ok(), "ordinary send on destroyed object should fail");
  expect(
      send.fault.has_value() &&
          send.fault->error_name == "DestroyedAccessError",
      "ordinary send on destroyed object should report DestroyedAccessError");
}

void test_runtime_lifecycle_dealloc_opcode_tombstones_instance_payload() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"mass"};

  BcCode dealloc;
  dealloc.code_id = 1;
  dealloc.kind = CodeKind::Method;
  dealloc.reg_count = 2;
  dealloc.instructions.push_back(
      {Opcode::ObjDealloc, {{1, false}, {0, false}}});
  dealloc.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode load_ivar;
  load_ivar.code_id = 2;
  load_ivar.kind = CodeKind::Method;
  load_ivar.reg_count = 2;
  load_ivar.instructions.push_back(
      {Opcode::LoadIvar, {{1, false}, {0, false}, {0, false}, {0, false}}});
  load_ivar.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {dealloc, load_ivar};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  instance->header.class_index = 0;
  instance->ivar_storage.push_back(amber::runtime::Value::integer(7));
  instance->ivars["mass"] = amber::runtime::Value::integer(7);

  const amber::runtime::ExecutionResult first = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(first.ok(), "OBJ_DEALLOC first execution should succeed");
  expect(first.value.is_bool() && first.value.as_bool(),
         "OBJ_DEALLOC should return true for first deallocation");
  expect(instance->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "OBJ_DEALLOC should transition object to deallocated state");
  expect((instance->header.flags & amber::runtime::kObjectFlagDead) != 0U,
         "OBJ_DEALLOC should set dead flag");
  expect(instance->header.shape != nullptr && instance->header.shape->dead,
         "OBJ_DEALLOC should rewrite shape to DeadShape");
  expect(instance->ivar_storage.empty() && instance->ivars.empty(),
         "OBJ_DEALLOC should release instance ivar payload");

  const amber::runtime::ExecutionResult second = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(second.ok(), "OBJ_DEALLOC second execution should succeed");
  expect(second.value.is_bool() && !second.value.as_bool(),
         "OBJ_DEALLOC should return false for already deallocated object");

  const amber::runtime::ExecutionResult load = amber::runtime::execute_code(
      module, 2, {amber::runtime::Value::instance(instance)});
  expect(!load.ok(), "ivar access on deallocated object should fail");
  expect(load.fault.has_value() &&
             load.fault->error_name == "UseAfterFreeError",
         "ivar access on deallocated object should report UseAfterFreeError");
}

void test_runtime_lifecycle_dealloc_clears_collection_payload() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"empty?"};

  BcCode dealloc;
  dealloc.code_id = 1;
  dealloc.kind = CodeKind::Method;
  dealloc.reg_count = 2;
  dealloc.instructions.push_back(
      {Opcode::ObjDealloc, {{1, false}, {0, false}}});
  dealloc.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode builtin_send;
  builtin_send.code_id = 2;
  builtin_send.kind = CodeKind::Method;
  builtin_send.reg_count = 2;
  builtin_send.instructions.push_back({Opcode::Send,
                                       {{1, false},
                                        {0, false},
                                        {0, false},
                                        {0, false},
                                        {0, false},
                                        {-1, true},
                                        {0, false}}});
  builtin_send.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {dealloc, builtin_send};

  amber::runtime::Value list = amber::runtime::make_list_value(
      {amber::runtime::Value::integer(1), amber::runtime::Value::integer(2)});
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list_ptr =
      list.as_list();

  const amber::runtime::ExecutionResult dealloc_result =
      amber::runtime::execute_code(module, 1, {list});
  expect(dealloc_result.ok(), "list OBJ_DEALLOC should succeed");
  expect(dealloc_result.value.is_bool() && dealloc_result.value.as_bool(),
         "list OBJ_DEALLOC should return true");
  expect(list_ptr->items.empty(), "list OBJ_DEALLOC should clear items");
  expect(list_ptr->header.lifetime_state ==
             amber::runtime::ObjectLifetimeState::Deallocated,
         "list OBJ_DEALLOC should mark list deallocated");

  const amber::runtime::ExecutionResult send =
      amber::runtime::execute_code(module, 2, {list});
  expect(!send.ok(), "builtin send on deallocated list should fail");
  expect(send.fault.has_value() &&
             send.fault->error_name == "UseAfterFreeError",
         "builtin send on deallocated list should report UseAfterFreeError");
}

void test_runtime_world_define_method_invalidates_send_cache() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "value"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod original;
  original.selector_sym_id = 1;
  original.owner_dispatch_ref = 0;
  original.signature_blob_id = 0;
  original.entry_code_id = 2;
  original.flags = 1;
  module.methods.push_back(original);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {1, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body_one;
  body_one.code_id = 2;
  body_one.kind = CodeKind::Method;
  body_one.reg_count = 1;
  body_one.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  body_one.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_two;
  body_two.code_id = 3;
  body_two.kind = CodeKind::Method;
  body_two.reg_count = 1;
  body_two.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  body_two.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_one, body_two};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  amber::runtime::RuntimeWorld world(module);
  expect(world.method_table_size(
             0, amber::runtime::MethodTableSide::Instance) == 1,
         "instance method table should include emitted method");
  expect(world.method_table_size(0, amber::runtime::MethodTableSide::Class) ==
             0,
         "class method table should be empty for instance-only owner");

  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok(), "define_method preflight send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "initial method should return original value");

  const std::uint64_t epoch_before = world.world_epoch();
  const std::uint64_t version_before = world.method_version(0);
  BcMethod replacement = original;
  replacement.entry_code_id = 3;
  const amber::runtime::ExecutionResult define_result =
      world.define_instance_method(0, replacement);
  expect(define_result.ok(), "runtime define_instance_method failed");
  expect(world.world_epoch() == epoch_before + 1,
         "define_method should bump world epoch");
  expect(world.method_version(0) == version_before + 1,
         "define_method should bump owner method version");
  expect(world.method_table_size(
             0, amber::runtime::MethodTableSide::Instance) == 1,
         "method replacement should keep a stable method-table slot");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok(), "define_method post-mutation send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "send cache should invalidate after method replacement");
}

void test_runtime_world_include_invalidates_send_cache() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "Older", "Newer", "label"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant older_path;
  older_path.kind = ConstantKind::Path;
  older_path.items = {1};
  module.const_pool.push_back(older_path);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant two;
  two.kind = ConstantKind::Integer;
  two.int_value = 2;
  module.const_pool.push_back(two);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 2;
  box.method_range_count = 0;
  box.direct_include_refs.push_back(1);
  module.classes.push_back(box);

  BcClass older;
  older.class_name_sym_id = 1;
  older.method_range_start = 0;
  older.method_range_count = 1;
  older.flags = kClassFlagMixin;
  module.classes.push_back(older);

  BcClass newer;
  newer.class_name_sym_id = 2;
  newer.method_range_start = 1;
  newer.method_range_count = 1;
  newer.flags = kClassFlagMixin;
  module.classes.push_back(newer);

  BcMethod older_method;
  older_method.selector_sym_id = 3;
  older_method.owner_dispatch_ref = 1;
  older_method.signature_blob_id = 0;
  older_method.entry_code_id = 2;
  older_method.flags = 1;
  module.methods.push_back(older_method);

  BcMethod newer_method = older_method;
  newer_method.owner_dispatch_ref = 2;
  newer_method.entry_code_id = 3;
  module.methods.push_back(newer_method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back({Opcode::Send,
                                 {{1, false},
                                  {0, false},
                                  {3, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode old_body;
  old_body.code_id = 2;
  old_body.kind = CodeKind::Method;
  old_body.reg_count = 1;
  old_body.instructions.push_back({Opcode::LoadK, {{0, false}, {2, false}}});
  old_body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode new_body;
  new_body.code_id = 3;
  new_body.kind = CodeKind::Method;
  new_body.reg_count = 1;
  new_body.instructions.push_back({Opcode::LoadK, {{0, false}, {3, false}}});
  new_body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, old_body, new_body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok(), "include preflight send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "static included mixin should answer before late include");

  const amber::runtime::ExecutionResult include_result =
      world.include_mixin(0, 2);
  expect(include_result.ok(), "runtime include_mixin failed");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok(), "include post-mutation send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "late include should dominate and invalidate cached dispatch");
}

void test_runtime_world_transaction_replaces_mixin_method_for_cached_class() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "Trait", "value"};
  append_path_const(&module, {});
  const std::uint32_t trait_ref = append_path_const(&module, {1});
  const std::uint32_t one_id = append_integer_const(&module, 1);
  const std::uint32_t two_id = append_integer_const(&module, 2);

  BcClass box;
  box.class_name_sym_id = 0;
  box.direct_include_refs.push_back(trait_ref);
  module.classes.push_back(box);

  BcClass trait;
  trait.class_name_sym_id = 1;
  trait.method_range_start = 0;
  trait.method_range_count = 1;
  trait.flags = kClassFlagMixin;
  module.classes.push_back(trait);

  BcMethod original;
  original.selector_sym_id = 2;
  original.owner_dispatch_ref = 1;
  original.entry_code_id = 2;
  original.flags = 1;
  module.methods.push_back(original);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back(send_instr(1, 0, 2, {}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body_one;
  body_one.code_id = 2;
  body_one.kind = CodeKind::Method;
  body_one.reg_count = 1;
  body_one.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {one_id, false}}});
  body_one.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_two;
  body_two.code_id = 3;
  body_two.kind = CodeKind::Method;
  body_two.reg_count = 1;
  body_two.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {two_id, false}}});
  body_two.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_one, body_two};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  amber::runtime::RuntimeWorld world(module);

  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok(), "mixin reopen preflight send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "static mixin method should answer before reopen");

  amber::runtime::RuntimeWorldTransaction tx;
  tx.target_kind = amber::runtime::RuntimeOwnerKind::Mixin;
  tx.target_index = 1;
  BcMethod replacement = original;
  replacement.entry_code_id = 3;
  tx.instance_methods.push_back(replacement);

  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::ExecutionResult committed =
      world.commit_transaction(tx);
  expect(committed.ok(), "mixin reopen transaction should commit");
  expect(world.world_epoch() == epoch_before + 1,
         "mixin reopen should bump world epoch once");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok(), "mixin reopen post-commit send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "mixin reopen should invalidate class receiver cache");
}

void test_runtime_world_transaction_rolls_back_on_invalid_include() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "Trait", "Plain", "value"};
  append_path_const(&module, {});
  const std::uint32_t one_id = append_integer_const(&module, 1);

  BcClass box;
  box.class_name_sym_id = 0;
  module.classes.push_back(box);

  BcClass trait;
  trait.class_name_sym_id = 1;
  trait.flags = kClassFlagMixin;
  module.classes.push_back(trait);

  BcClass plain;
  plain.class_name_sym_id = 2;
  module.classes.push_back(plain);

  BcMethod method;
  method.selector_sym_id = 3;
  method.entry_code_id = 2;

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back(send_instr(1, 0, 3, {}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {one_id, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body};

  amber::runtime::RuntimeWorld world(module);
  const std::uint64_t epoch_before = world.world_epoch();
  amber::runtime::RuntimeWorldTransaction tx;
  tx.target_kind = amber::runtime::RuntimeOwnerKind::Class;
  tx.target_index = 0;
  tx.instance_methods.push_back(method);
  tx.include_indices.push_back(2);

  const amber::runtime::ExecutionResult failed = world.commit_transaction(tx);
  expect(!failed.ok(), "invalid include transaction should fail");
  expect(failed.fault.has_value() && failed.fault->error_name == "TypeError",
         "invalid include should report TypeError");
  expect(world.world_epoch() == epoch_before,
         "failed transaction should not bump world epoch");

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult send =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(!send.ok(), "rolled-back method should not be visible");
  expect(send.fault.has_value() && send.fault->error_name == "NoMethodError",
         "rolled-back method should leave dispatch unchanged");
}

void test_runtime_world_freeze_rejects_world_mutation_but_keeps_send() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "value"};
  append_path_const(&module, {});
  const std::uint32_t one_id = append_integer_const(&module, 1);
  const std::uint32_t two_id = append_integer_const(&module, 2);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod original;
  original.selector_sym_id = 1;
  original.owner_dispatch_ref = 0;
  original.entry_code_id = 2;
  original.flags = 1;
  module.methods.push_back(original);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back(send_instr(1, 0, 1, {}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode body_one;
  body_one.code_id = 2;
  body_one.kind = CodeKind::Method;
  body_one.reg_count = 1;
  body_one.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {one_id, false}}});
  body_one.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode body_two;
  body_two.code_id = 3;
  body_two.kind = CodeKind::Method;
  body_two.reg_count = 1;
  body_two.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {two_id, false}}});
  body_two.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, body_one, body_two};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  amber::runtime::RuntimeWorld world(module);
  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok() && before.value.is_integer() &&
             before.value.as_integer() == 1,
         "pre-freeze send should use original method");

  const amber::runtime::ExecutionResult frozen = world.freeze_world();
  expect(frozen.ok(), "freeze_world should succeed");
  expect(world.world_state() == amber::runtime::RuntimeWorldState::Frozen,
         "world should report frozen state");

  BcMethod replacement = original;
  replacement.entry_code_id = 3;
  const amber::runtime::ExecutionResult rejected =
      world.define_instance_method(0, replacement);
  expect(!rejected.ok(), "post-freeze define_method should fail");
  expect(rejected.fault.has_value() &&
             rejected.fault->error_name == "WorldFrozenError",
         "post-freeze mutation should report WorldFrozenError");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok() && after.value.is_integer() &&
             after.value.as_integer() == 1,
         "ordinary send should remain legal after freeze");
}

void test_runtime_world_transaction_rejects_superclass_mismatch() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Parent", "Other", "Child"};
  const std::uint32_t parent_ref = append_path_const(&module, {0});
  const std::uint32_t other_ref = append_path_const(&module, {1});

  BcClass parent;
  parent.class_name_sym_id = 0;
  module.classes.push_back(parent);

  BcClass other;
  other.class_name_sym_id = 1;
  module.classes.push_back(other);

  BcClass child;
  child.class_name_sym_id = 2;
  child.has_superclass_ref = true;
  child.superclass_ref = parent_ref;
  module.classes.push_back(child);

  amber::runtime::RuntimeWorld world(module);
  const std::uint64_t epoch_before = world.world_epoch();
  amber::runtime::RuntimeWorldTransaction tx;
  tx.target_kind = amber::runtime::RuntimeOwnerKind::Class;
  tx.target_index = 2;
  tx.has_superclass_ref = true;
  tx.superclass_ref = other_ref;

  const amber::runtime::ExecutionResult failed = world.commit_transaction(tx);
  expect(!failed.ok(), "superclass mismatch transaction should fail");
  expect(failed.fault.has_value() &&
             failed.fault->error_name == "SuperclassMismatchError",
         "superclass mismatch should report SuperclassMismatchError");
  expect(world.world_epoch() == epoch_before,
         "superclass mismatch should not publish transaction");
}

void test_runtime_world_transaction_rejects_include_cycle_before_commit() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"MixA", "MixB"};

  BcClass mix_a;
  mix_a.class_name_sym_id = 0;
  mix_a.flags = kClassFlagMixin;
  module.classes.push_back(mix_a);

  BcClass mix_b;
  mix_b.class_name_sym_id = 1;
  mix_b.flags = kClassFlagMixin;
  module.classes.push_back(mix_b);

  amber::runtime::RuntimeWorld world(module);
  amber::runtime::RuntimeWorldTransaction first;
  first.target_kind = amber::runtime::RuntimeOwnerKind::Mixin;
  first.target_index = 0;
  first.include_indices.push_back(1);
  expect(world.commit_transaction(first).ok(),
         "initial acyclic mixin include should commit");

  const std::uint64_t epoch_before = world.world_epoch();
  amber::runtime::RuntimeWorldTransaction cycle;
  cycle.target_kind = amber::runtime::RuntimeOwnerKind::Mixin;
  cycle.target_index = 1;
  cycle.include_indices.push_back(0);

  const amber::runtime::ExecutionResult failed =
      world.commit_transaction(cycle);
  expect(!failed.ok(), "cyclic include transaction should fail");
  expect(failed.fault.has_value() &&
             failed.fault->error_name == "IncludeCycleError",
         "cyclic include should report IncludeCycleError");
  expect(world.world_epoch() == epoch_before,
         "cyclic include should not bump world epoch");
}

void test_runtime_world_extend_invalidates_class_side_send_cache() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "Older", "Newer", "label"};
  append_path_const(&module, {});
  const std::uint32_t older_ref = append_path_const(&module, {1});
  const std::uint32_t one_id = append_integer_const(&module, 1);
  const std::uint32_t two_id = append_integer_const(&module, 2);

  BcClass box;
  box.class_name_sym_id = 0;
  box.direct_extend_refs.push_back(older_ref);
  module.classes.push_back(box);

  BcClass older;
  older.class_name_sym_id = 1;
  older.method_range_start = 0;
  older.method_range_count = 1;
  older.flags = kClassFlagMixin;
  module.classes.push_back(older);

  BcClass newer;
  newer.class_name_sym_id = 2;
  newer.method_range_start = 1;
  newer.method_range_count = 1;
  newer.flags = kClassFlagMixin;
  module.classes.push_back(newer);

  BcMethod older_method;
  older_method.selector_sym_id = 3;
  older_method.owner_dispatch_ref = 1;
  older_method.entry_code_id = 2;
  older_method.flags = 1;
  module.methods.push_back(older_method);

  BcMethod newer_method = older_method;
  newer_method.owner_dispatch_ref = 2;
  newer_method.entry_code_id = 3;
  module.methods.push_back(newer_method);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 2;
  caller.instructions.push_back(send_instr(1, 0, 3, {}, -1, 0));
  caller.instructions.push_back({Opcode::Return, {{1, false}}});

  BcCode old_body;
  old_body.code_id = 2;
  old_body.kind = CodeKind::Method;
  old_body.reg_count = 1;
  old_body.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {one_id, false}}});
  old_body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode new_body;
  new_body.code_id = 3;
  new_body.kind = CodeKind::Method;
  new_body.reg_count = 1;
  new_body.instructions.push_back(
      {Opcode::LoadK, {{0, false}, {two_id, false}}});
  new_body.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {caller, old_body, new_body};

  amber::runtime::RuntimeWorld world(module);
  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::class_object(0)});
  expect(before.ok(), "extend preflight class-side send failed");
  expect(before.value.is_integer() && before.value.as_integer() == 1,
         "static extend should answer before late extend");

  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::ExecutionResult extended = world.extend_mixin(0, 2);
  expect(extended.ok(), "runtime extend_mixin should commit");
  expect(world.world_epoch() == epoch_before + 1,
         "late extend should bump world epoch");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::class_object(0)});
  expect(after.ok(), "extend post-mutation class-side send failed");
  expect(after.value.is_integer() && after.value.as_integer() == 2,
         "late extend should dominate and invalidate class-side cache");
}

void test_runtime_reflection_mirrors_are_read_only_stable_and_ordered() {
  using namespace amber::bytecode;

  BcModule module;
  module.format_version = {1, 0};
  module.language_version = {1, 0};
  module.symbols = {"Box", "Trait", "Later", "zeta", "alpha", "beta"};

  const std::uint32_t package_key = append_string(&module, "amber.package");
  const std::uint32_t package_value = append_string(&module, "reflect.demo");
  const std::uint32_t dependency_name = append_string(&module, "dep.core");
  const std::uint32_t export_kind = append_string(&module, "class");
  module.attrs.push_back({package_key, package_value});

  DepEntry dependency;
  dependency.module_name_str_id = dependency_name;
  dependency.required_format = {1, 0};
  dependency.min_language_version = {1, 0};
  module.dependencies.push_back(dependency);
  module.exports.push_back({0, export_kind, 0, 0});

  const std::uint32_t trait_ref = append_path_const(&module, {1});

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 2;
  box.direct_include_refs.push_back(trait_ref);
  module.classes.push_back(box);

  BcClass trait;
  trait.class_name_sym_id = 1;
  trait.method_range_start = 2;
  trait.method_range_count = 1;
  trait.flags = kClassFlagMixin;
  module.classes.push_back(trait);

  BcClass later;
  later.class_name_sym_id = 2;
  later.method_range_start = 3;
  later.method_range_count = 1;
  later.flags = kClassFlagMixin;
  module.classes.push_back(later);

  BcMethod zeta;
  zeta.selector_sym_id = 3;
  zeta.owner_dispatch_ref = 0;
  zeta.entry_code_id = 10;
  zeta.flags = 1;
  module.methods.push_back(zeta);

  BcMethod alpha = zeta;
  alpha.selector_sym_id = 4;
  alpha.entry_code_id = 11;
  module.methods.push_back(alpha);

  BcMethod beta = zeta;
  beta.selector_sym_id = 5;
  beta.owner_dispatch_ref = 1;
  beta.entry_code_id = 12;
  module.methods.push_back(beta);

  BcMethod later_method = zeta;
  later_method.selector_sym_id = 5;
  later_method.owner_dispatch_ref = 2;
  later_method.entry_code_id = 13;
  module.methods.push_back(later_method);

  BcCode zeta_code;
  zeta_code.code_id = 10;
  zeta_code.kind = CodeKind::Method;
  zeta_code.source_spans.push_back(
      {0, 1, {"reflect.am", {20, 3, 100}, {20, 8, 105}}});
  BcCode alpha_code;
  alpha_code.code_id = 11;
  alpha_code.kind = CodeKind::Method;
  alpha_code.source_spans.push_back(
      {0, 1, {"reflect.am", {12, 5, 50}, {12, 10, 55}}});
  BcCode beta_code;
  beta_code.code_id = 12;
  beta_code.kind = CodeKind::Method;
  beta_code.source_spans.push_back(
      {0, 1, {"reflect.am", {30, 3, 150}, {30, 8, 155}}});
  BcCode later_code;
  later_code.code_id = 13;
  later_code.kind = CodeKind::Method;
  later_code.source_spans.push_back(
      {0, 1, {"reflect.am", {40, 3, 200}, {40, 8, 205}}});
  module.code_objects = {zeta_code, alpha_code, beta_code, later_code};

  amber::runtime::RuntimeWorld world(module);
  amber::runtime::RuntimeWorldMirror snapshot = world.world_mirror();
  expect(snapshot.read_only, "world mirror should advertise read-only state");
  expect(snapshot.package.read_only, "package mirror should be read-only");
  expect(snapshot.package.name == "reflect.demo",
         "package mirror should expose package name attr");
  expect(snapshot.package.dependencies.size() == 1 &&
             snapshot.package.dependencies[0].module_name == "dep.core",
         "package mirror should expose deterministic dependencies");
  expect(snapshot.package.exports.size() == 1 &&
             snapshot.package.exports[0].public_name == "Box",
         "package mirror should expose deterministic exports");
  expect(snapshot.owners.size() == 3, "world mirror should expose all owners");

  std::optional<amber::runtime::RuntimeOwnerMirror> box_mirror =
      world.class_mirror(0);
  expect(box_mirror.has_value(), "class mirror should resolve class owner");
  expect(!world.mixin_mirror(0).has_value(),
         "mixin mirror should reject class owner");
  expect(world.mixin_mirror(1).has_value(),
         "mixin mirror should resolve mixin owner");
  expect(box_mirror->read_only, "owner mirror should be read-only");
  expect(box_mirror->kind == amber::runtime::RuntimeOwnerKind::Class,
         "owner mirror should report class kind");
  expect(box_mirror->instance_methods.size() == 2,
         "class mirror should expose instance method table");
  expect(box_mirror->instance_methods[0].selector == "alpha" &&
             box_mirror->instance_methods[1].selector == "zeta",
         "method mirrors should be sorted by selector name");
  expect(box_mirror->instance_methods[0].read_only,
         "method mirror should be read-only");
  expect(box_mirror->instance_methods[0].source_location.present &&
             box_mirror->instance_methods[0].source_location.file ==
                 "reflect.am" &&
             box_mirror->instance_methods[0].source_location.line == 12,
         "method mirror should expose source location");
  expect(box_mirror->direct_includes.size() == 1 &&
             box_mirror->direct_includes[0].name == "Trait" &&
             !box_mirror->direct_includes[0].dynamic,
         "class mirror should expose static direct includes");

  snapshot.owners[0].instance_methods.clear();
  expect(world.class_mirror(0)->instance_methods.size() == 2,
         "mutating a mirror copy should not mutate runtime state");

  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::ExecutionResult included = world.include_mixin(0, 2);
  expect(included.ok(), "late include for mirror test should commit");
  std::optional<amber::runtime::RuntimeOwnerMirror> after_include =
      world.class_mirror(0);
  expect(after_include.has_value(),
         "class mirror after include should resolve");
  expect(after_include->world_epoch == epoch_before + 1,
         "owner mirror should carry current world epoch");
  expect(after_include->direct_includes.size() == 2 &&
             after_include->direct_includes[1].name == "Later" &&
             after_include->direct_includes[1].dynamic,
         "class mirror should expose late dynamic includes after static ones");
  expect(snapshot.owners[0].direct_includes.size() == 1,
         "old world mirror snapshot should stay stable after mutation");
}

void test_runtime_package_reload_swaps_compatible_package_atomically() {
  const amber::pkg::PackageArtifact original =
      make_reload_artifact(make_reload_module(1));
  const amber::pkg::PackageArtifact replacement =
      make_reload_artifact(make_reload_module(2));
  amber::runtime::RuntimeWorld world(original);

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult before =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(before.ok() && before.value.is_integer() &&
             before.value.as_integer() == 1,
         "package reload preflight should execute original method body");

  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::RuntimePackageReloadResult reloaded =
      world.reload_package_artifact(replacement);
  expect(reloaded.ok && reloaded.swapped,
         "compatible package reload should swap active package");
  expect(world.world_epoch() == epoch_before + 1,
         "compatible package reload should bump world epoch once");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok() && after.value.is_integer() &&
             after.value.as_integer() == 2,
         "package reload should execute replacement method body");
}

void test_runtime_package_reload_rejects_incompatible_surface_without_swap() {
  const amber::pkg::PackageArtifact original =
      make_reload_artifact(make_reload_module(1));
  const amber::pkg::PackageArtifact removed_export =
      make_reload_artifact(make_reload_module(2, false));
  const amber::pkg::PackageArtifact arity_change =
      make_reload_artifact(make_reload_module(3, true, true));
  amber::runtime::RuntimeWorld world(original);

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::RuntimePackageReloadResult rejected_export =
      world.reload_package_artifact(removed_export);
  expect(!rejected_export.ok && !rejected_export.swapped,
         "export-surface reload should be rejected");
  expect(!rejected_export.diagnostics.empty() &&
             rejected_export.diagnostics[0].error_name ==
                 "ReloadIncompatibleError",
         "export-surface reload should report ReloadIncompatibleError");
  expect(world.world_epoch() == epoch_before,
         "rejected export-surface reload should not bump epoch");

  const amber::runtime::RuntimePackageReloadResult rejected_arity =
      world.reload_package_artifact(arity_change);
  expect(!rejected_arity.ok && !rejected_arity.swapped,
         "selector/arity reload should be rejected");
  expect(!rejected_arity.diagnostics.empty() &&
             rejected_arity.diagnostics[0].error_name ==
                 "ReloadIncompatibleError",
         "selector/arity reload should report ReloadIncompatibleError");

  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok() && after.value.is_integer() &&
             after.value.as_integer() == 1,
         "incompatible reload should leave original method body active");
}

void test_runtime_package_reload_rejects_frozen_world_without_swap() {
  const amber::pkg::PackageArtifact original =
      make_reload_artifact(make_reload_module(1));
  const amber::pkg::PackageArtifact replacement =
      make_reload_artifact(make_reload_module(2));
  amber::runtime::RuntimeWorld world(original);

  expect(world.freeze_world().ok(), "freeze before reload should succeed");
  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::RuntimePackageReloadResult reloaded =
      world.reload_package_artifact(replacement);
  expect(!reloaded.ok && !reloaded.swapped,
         "frozen package reload should be rejected");
  expect(!reloaded.diagnostics.empty() &&
             reloaded.diagnostics[0].error_name == "WorldFrozenError",
         "frozen package reload should report WorldFrozenError");
  expect(world.world_epoch() == epoch_before,
         "frozen reload rejection should not bump epoch");

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok() && after.value.is_integer() &&
             after.value.as_integer() == 1,
         "frozen reload rejection should leave original method body active");
}

void test_runtime_package_reload_rolls_back_failed_decode() {
  const amber::pkg::PackageArtifact original =
      make_reload_artifact(make_reload_module(1));
  amber::pkg::PackageArtifact broken =
      make_reload_artifact(make_reload_module(2));
  broken.modules[0].bytes = {0x00};
  amber::runtime::RuntimeWorld world(original);

  const std::uint64_t epoch_before = world.world_epoch();
  const amber::runtime::RuntimePackageReloadResult reloaded =
      world.reload_package_artifact(broken);
  expect(!reloaded.ok && !reloaded.swapped,
         "broken package reload should fail before swap");
  expect(!reloaded.diagnostics.empty() &&
             reloaded.diagnostics[0].error_name == "BytecodeVerificationError",
         "broken package reload should report bytecode verification failure");
  expect(world.world_epoch() == epoch_before,
         "broken reload should not bump epoch");

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult after =
      world.execute(1, {amber::runtime::Value::instance(instance)});
  expect(after.ok() && after.value.is_integer() &&
             after.value.as_integer() == 1,
         "broken reload should leave original method body active");
}

void test_manual_pattern_deconstruct_protocol_sequence() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Pair", "deconstruct"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant one;
  one.kind = ConstantKind::Integer;
  one.int_value = 1;
  module.const_pool.push_back(one);

  Constant nine;
  nine.kind = ConstantKind::Integer;
  nine.int_value = 9;
  module.const_pool.push_back(nine);

  Constant zero;
  zero.kind = ConstantKind::Integer;
  zero.int_value = 0;
  module.const_pool.push_back(zero);

  BcClass pair;
  pair.class_name_sym_id = 0;
  pair.method_range_start = 0;
  pair.method_range_count = 1;
  module.classes.push_back(pair);

  BcMethod deconstruct;
  deconstruct.selector_sym_id = 1;
  deconstruct.owner_dispatch_ref = 0;
  deconstruct.signature_blob_id = 0;
  deconstruct.entry_code_id = 2;
  deconstruct.flags = 1;
  module.methods.push_back(deconstruct);

  BcCode probe;
  probe.code_id = 1;
  probe.kind = CodeKind::Method;
  probe.reg_count = 5;
  probe.instructions.push_back(
      {Opcode::PPrepSeq, {{1, false}, {0, false}, {0, false}, {6, false}}});
  probe.instructions.push_back(
      {Opcode::PCheckLenEq, {{1, false}, {2, false}, {6, false}}});
  probe.instructions.push_back(
      {Opcode::PGetIndex, {{2, false}, {1, false}, {0, false}}});
  probe.instructions.push_back(
      {Opcode::PCheckEq, {{2, false}, {1, false}, {6, false}}});
  probe.instructions.push_back(
      {Opcode::PGetIndex, {{3, false}, {1, false}, {1, false}}});
  probe.instructions.push_back({Opcode::Return, {{3, false}}});
  probe.instructions.push_back({Opcode::LoadK, {{4, false}, {3, false}}});
  probe.instructions.push_back({Opcode::Return, {{4, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 4;
  body.instructions.push_back({Opcode::LoadK, {{0, false}, {1, false}}});
  body.instructions.push_back({Opcode::LoadK, {{1, false}, {2, false}}});
  body.instructions.push_back(
      {Opcode::MakeList, {{2, false}, {0, false}, {2, false}}});
  body.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects = {probe, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "sequence deconstruct protocol execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 9,
         "P_PREP_SEQ should use object deconstruct protocol");
}

void test_manual_pattern_deconstruct_protocol_map() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Payload", "deconstruct_keys", "a", "keys"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant keyset;
  keyset.kind = ConstantKind::KeySet;
  keyset.items = {2};
  module.const_pool.push_back(keyset);

  Constant seven;
  seven.kind = ConstantKind::Integer;
  seven.int_value = 7;
  module.const_pool.push_back(seven);

  Constant zero;
  zero.kind = ConstantKind::Integer;
  zero.int_value = 0;
  module.const_pool.push_back(zero);

  BcClass payload;
  payload.class_name_sym_id = 0;
  payload.method_range_start = 0;
  payload.method_range_count = 1;
  module.classes.push_back(payload);

  BcMethod deconstruct_keys;
  deconstruct_keys.selector_sym_id = 1;
  deconstruct_keys.owner_dispatch_ref = 0;
  deconstruct_keys.signature_blob_id = 0;
  deconstruct_keys.entry_code_id = 2;
  deconstruct_keys.flags = 1;
  deconstruct_keys.params.push_back({3, 0, 0});
  module.methods.push_back(deconstruct_keys);

  BcCode probe;
  probe.code_id = 1;
  probe.kind = CodeKind::Method;
  probe.reg_count = 4;
  probe.instructions.push_back(
      {Opcode::PPrepMap,
       {{1, false}, {0, false}, {1, false}, {0, false}, {4, false}}});
  probe.instructions.push_back(
      {Opcode::PHasKey, {{1, false}, {2, false}, {4, false}}});
  probe.instructions.push_back(
      {Opcode::PGetKey, {{2, false}, {1, false}, {2, false}}});
  probe.instructions.push_back({Opcode::Return, {{2, false}}});
  probe.instructions.push_back({Opcode::LoadK, {{3, false}, {3, false}}});
  probe.instructions.push_back({Opcode::Return, {{3, false}}});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 3;
  body.instructions.push_back({Opcode::LoadK, {{1, false}, {2, false}}});
  body.instructions.push_back(
      {Opcode::MakeMap, {{2, false}, {1, false}, {2, false}, {1, false}}});
  body.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects = {probe, body};

  auto instance =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  instance->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(instance)});
  expect(exec.ok(), "map deconstruct_keys protocol execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 7,
         "P_PREP_MAP should use object deconstruct_keys protocol");
}

void test_manual_raise_handler_table_recovers() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Boom"};

  Constant recovered;
  recovered.kind = ConstantKind::Integer;
  recovered.int_value = 42;
  module.const_pool.push_back(recovered);

  BcClass boom;
  boom.class_name_sym_id = 0;
  module.classes.push_back(boom);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({Opcode::Raise, {{0, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  code.handler_table.push_back({0, 1, 1, 2, 0});

  BcCode handler;
  handler.code_id = 2;
  handler.kind = CodeKind::Rescue;
  handler.reg_count = 2;
  handler.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  handler.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {code, handler};

  auto exception =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  exception->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(exception)});
  expect(exec.ok(), "RAISE handler execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "rescue handler should recover with its return value");
}

void test_manual_raise_unwinds_closure_to_outer_handler() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Boom"};

  Constant recovered;
  recovered.kind = ConstantKind::Integer;
  recovered.int_value = 99;
  module.const_pool.push_back(recovered);

  BcClass boom;
  boom.class_name_sym_id = 0;
  module.classes.push_back(boom);

  BcCode outer;
  outer.code_id = 1;
  outer.kind = CodeKind::Method;
  outer.reg_count = 3;
  outer.instructions.push_back(
      {Opcode::MakeClosure, {{1, false}, {2, false}, {0, false}}});
  outer.instructions.push_back({Opcode::Call,
                                {{2, false},
                                 {1, false},
                                 {1, false},
                                 {0, false},
                                 {0, false},
                                 {-1, true},
                                 {0, false}}});
  outer.instructions.push_back({Opcode::Return, {{2, false}}});
  outer.instructions.push_back({Opcode::Return, {{0, false}}});
  outer.handler_table.push_back({1, 2, 3, 3, 0});

  BcCode inner;
  inner.code_id = 2;
  inner.kind = CodeKind::Block;
  inner.reg_count = 1;
  inner.instructions.push_back({Opcode::Raise, {{0, false}}});
  inner.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode handler;
  handler.code_id = 3;
  handler.kind = CodeKind::Rescue;
  handler.reg_count = 2;
  handler.instructions.push_back({Opcode::LoadK, {{1, false}, {0, false}}});
  handler.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {outer, inner, handler};

  auto exception =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  exception->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(exception)});
  expect(exec.ok(), "closure RAISE unwind execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 99,
         "outer handler should catch exception from active closure call");
}

void test_manual_raise_unwinds_method_send_to_outer_handler() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Box", "explode"};

  Constant empty_signature;
  empty_signature.kind = ConstantKind::Path;
  module.const_pool.push_back(empty_signature);

  Constant recovered;
  recovered.kind = ConstantKind::Integer;
  recovered.int_value = 17;
  module.const_pool.push_back(recovered);

  BcClass box;
  box.class_name_sym_id = 0;
  box.method_range_start = 0;
  box.method_range_count = 1;
  module.classes.push_back(box);

  BcMethod explode;
  explode.selector_sym_id = 1;
  explode.owner_dispatch_ref = 0;
  explode.signature_blob_id = 0;
  explode.entry_code_id = 2;
  explode.flags = 1;
  module.methods.push_back(explode);

  BcCode caller;
  caller.code_id = 1;
  caller.kind = CodeKind::Method;
  caller.reg_count = 3;
  caller.instructions.push_back({Opcode::Send,
                                 {{2, false},
                                  {0, false},
                                  {1, false},
                                  {0, false},
                                  {0, false},
                                  {-1, true},
                                  {0, false}}});
  caller.instructions.push_back({Opcode::Return, {{2, false}}});
  caller.instructions.push_back({Opcode::Return, {{0, false}}});
  caller.handler_table.push_back({0, 1, 2, 3, 0});

  BcCode body;
  body.code_id = 2;
  body.kind = CodeKind::Method;
  body.reg_count = 1;
  body.instructions.push_back({Opcode::LoadSelf, {{0, false}}});
  body.instructions.push_back({Opcode::Raise, {{0, false}}});
  body.instructions.push_back({Opcode::Return, {{0, false}}});

  BcCode handler;
  handler.code_id = 3;
  handler.kind = CodeKind::Rescue;
  handler.reg_count = 2;
  handler.instructions.push_back({Opcode::LoadK, {{1, false}, {1, false}}});
  handler.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects = {caller, body, handler};

  auto receiver =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  receiver->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(receiver)});
  expect(exec.ok(), "method SEND RAISE unwind execution failed");
  expect(exec.value.is_integer() && exec.value.as_integer() == 17,
         "outer handler should catch exception from active method SEND");
}

void test_manual_raise_unhandled_fault_trace() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Boom"};

  BcClass boom;
  boom.class_name_sym_id = 0;
  module.classes.push_back(boom);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 1;
  code.instructions.push_back({Opcode::Raise, {{0, false}}});
  code.source_spans.push_back({0, 1, {"raise.am", {3, 5, 10}, {3, 15, 20}}});
  module.code_objects.push_back(code);

  auto exception =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  exception->class_index = 0;
  const amber::runtime::ExecutionResult exec = amber::runtime::execute_code(
      module, 1, {amber::runtime::Value::instance(exception)});
  expect(!exec.ok(), "unhandled RAISE should fail");
  expect(exec.fault.has_value() && exec.fault->error_name == "Boom",
         "unhandled RAISE should use exception class name");
  expect(!exec.fault->trace.empty() && exec.fault->trace[0].code_id == 1 &&
             exec.fault->trace[0].pc == 0 && exec.fault->trace[0].line == 3,
         "unhandled RAISE should include source trace frame");
  expect(exec.fault->trace[0].byte_start == 10 &&
             exec.fault->trace[0].byte_end == 20 &&
             exec.fault->trace[0].column == 5 &&
             exec.fault->trace[0].column_end == 15 &&
             exec.fault->trace[0].generated_kind == "direct",
         "unhandled RAISE should include structured source-map span");
  expect(exec.fault->trace_text.find("Boom:") != std::string::npos &&
             exec.fault->trace_text.find("c1:0") != std::string::npos,
         "unhandled RAISE should include human-readable trace text");
}

void test_source_try_rescue_ensure_execution() {
  const std::string binding_source = "try:\n"
                                     "  raise \"boom\"\n"
                                     "rescue |e|:\n"
                                     "  e\n";
  const amber::bytecode::EmitResult binding_emit = emit_ok(binding_source);
  const amber::bytecode::DecodeResult binding_decoded =
      amber::bytecode::deserialize_module(
          amber::bytecode::serialize_module(binding_emit.module));
  expect(binding_decoded.ok(),
         amber::bytecode::verify_errors_to_json(binding_decoded.errors));
  const amber::runtime::ExecutionResult binding_exec =
      amber::runtime::execute_code(binding_decoded.module,
                                   binding_decoded.module.init.entry_code_id);
  expect(binding_exec.ok(), "try rescue binding execution failed");
  expect(string_value_text_or_die(binding_exec.value, binding_decoded.module,
                                  binding_exec) == "boom",
         "rescue binding should receive raised value");
  const amber::runtime::ExecutionLocal *binding_local =
      execution_local_by_name(binding_exec, "e");
  expect(binding_local != nullptr && binding_local->initialized,
         "rescue binding appears initialized in execution locals");

  amber::runtime::ExecutionResult typed_rescue =
      execute_emitted_init("class Boom:\n"
                           "  def init():\n"
                           "    pass\n"
                           "try:\n"
                           "  raise Boom()\n"
                           "rescue Boom:\n"
                           "  7\n");
  expect(typed_rescue.ok(), "typed rescue execution failed");
  expect(typed_rescue.value.is_integer() &&
             typed_rescue.value.as_integer() == 7,
         "typed rescue matcher should catch matching class instances");

  amber::runtime::ExecutionResult normal_ensure =
      execute_emitted_init("x = 0\n"
                           "try:\n"
                           "  5\n"
                           "ensure:\n"
                           "  x = 9\n");
  expect(normal_ensure.ok(), "normal ensure execution failed");
  expect(normal_ensure.value.is_integer() &&
             normal_ensure.value.as_integer() == 5,
         "ensure must not replace normal try result");
  const amber::runtime::ExecutionLocal *x_local =
      execution_local_by_name(normal_ensure, "x");
  expect(x_local != nullptr && x_local->value.is_integer() &&
             x_local->value.as_integer() == 9,
         "ensure normal path side effect is visible");

  amber::runtime::ExecutionResult nested_ensure =
      execute_emitted_init("x = 0\n"
                           "try:\n"
                           "  try:\n"
                           "    raise \"boom\"\n"
                           "  ensure:\n"
                           "    x = 9\n"
                           "rescue:\n"
                           "  x\n");
  expect(nested_ensure.ok(), "nested ensure rethrow execution failed");
  expect(nested_ensure.value.is_integer() &&
             nested_ensure.value.as_integer() == 9,
         "ensure must run before exception continues to outer rescue");

  amber::runtime::ExecutionResult break_ensure =
      execute_emitted_init("x = 0\n"
                           "loop:\n"
                           "  try:\n"
                           "    break 5\n"
                           "  ensure:\n"
                           "    x = 9\n");
  expect(break_ensure.ok(), "break ensure execution failed");
  expect(break_ensure.value.is_integer() &&
             break_ensure.value.as_integer() == 5,
         "break value should survive ensure");
  const amber::runtime::ExecutionLocal *break_x_local =
      execution_local_by_name(break_ensure, "x");
  expect(break_x_local != nullptr && break_x_local->value.is_integer() &&
             break_x_local->value.as_integer() == 9,
         "ensure must run before break exits protected body");
}

void test_native_error_inherited_rescue_execution() {
  amber::runtime::ExecutionResult inherited =
      execute_emitted_init("try:\n"
                           "  raise JsonParseError(\"bad json\")\n"
                           "rescue JsonError |e|:\n"
                           "  if JsonError === e and JsonParseError === e:\n"
                           "    7\n"
                           "  else:\n"
                           "    0\n");
  expect(inherited.ok() && inherited.value.is_integer() &&
             inherited.value.as_integer() == 7,
         "native parent error class should catch registered subclass");

  amber::runtime::ExecutionResult sibling =
      execute_emitted_init("try:\n"
                           "  try:\n"
                           "    raise JsonParseError(\"bad json\")\n"
                           "  rescue JsonGenerateError:\n"
                           "    0\n"
                           "rescue JsonError:\n"
                           "  7\n");
  expect(sibling.ok() && sibling.value.is_integer() &&
             sibling.value.as_integer() == 7,
         "native sibling error class should not catch");

  amber::runtime::ExecutionResult url =
      execute_emitted_init("try:\n"
                           "  raise UrlParseError(\"bad url\")\n"
                           "rescue UrlError |e|:\n"
                           "  if UrlError === e and UrlParseError === e:\n"
                           "    7\n"
                           "  else:\n"
                           "    0\n");
  expect(url.ok() && url.value.is_integer() && url.value.as_integer() == 7,
         "URL parent error class should catch registered subclass");

  amber::runtime::ExecutionResult root =
      execute_emitted_init("try:\n"
                           "  raise TypeError(\"bad type\")\n"
                           "rescue Exception:\n"
                           "  7\n");
  expect(root.ok() && root.value.is_integer() && root.value.as_integer() == 7,
         "Exception should catch existing native error classes");
}

void test_source_throw_catch_execution() {
  amber::runtime::ExecutionResult deep =
      execute_emitted_init("def deep_nested_code():\n"
                           "  throw :enough, 42\n"
                           "\n"
                           "catch(:enough):\n"
                           "  deep_nested_code()\n");
  expect(deep.ok(), "deep throw/catch execution failed");
  expect(deep.value.is_integer() && deep.value.as_integer() == 42,
         "catch should return thrown payload from deep call");

  amber::runtime::ExecutionResult bare =
      execute_emitted_init("catch :enough:\n"
                           "  throw :enough, 7\n");
  expect(bare.ok(), "bare catch execution failed");
  expect(bare.value.is_integer() && bare.value.as_integer() == 7,
         "bare catch should return thrown payload");

  amber::runtime::ExecutionResult null_payload =
      execute_emitted_init("catch :enough:\n"
                           "  throw :enough\n");
  expect(null_payload.ok(), "throw without payload execution failed");
  expect(null_payload.value.is_null(),
         "throw without payload should deliver null");

  amber::runtime::ExecutionResult evaluated_tag_once =
      execute_emitted_init("tag = :enough\n"
                           "catch tag:\n"
                           "  tag = :other\n"
                           "  throw :enough, 21\n");
  expect(evaluated_tag_once.ok(), "catch tag snapshot execution failed");
  expect(evaluated_tag_once.value.is_integer() &&
             evaluated_tag_once.value.as_integer() == 21,
         "catch should use tag value evaluated before body");

  amber::runtime::ExecutionResult nested =
      execute_emitted_init("catch :outer:\n"
                           "  catch :inner:\n"
                           "    throw :outer, 33\n");
  expect(nested.ok(), "mismatched inner catch execution failed");
  expect(nested.value.is_integer() && nested.value.as_integer() == 33,
         "mismatched catch should continue to outer matching catch");

  amber::runtime::ExecutionResult not_rescued =
      execute_emitted_init("catch :enough:\n"
                           "  try:\n"
                           "    throw :enough, 17\n"
                           "  rescue:\n"
                           "    99\n");
  expect(not_rescued.ok(), "throw through rescue execution failed");
  expect(not_rescued.value.is_integer() && not_rescued.value.as_integer() == 17,
         "rescue must not catch throw/catch control flow");

  amber::runtime::ExecutionResult ensure_result =
      execute_emitted_init("x = 0\n"
                           "y = 0\n"
                           "res = catch :enough:\n"
                           "  try:\n"
                           "    throw :enough, 11\n"
                           "  ensure:\n"
                           "    x = 5\n"
                           "    y = 1 + 2\n"
                           "res + x + y\n");
  expect(ensure_result.ok(), "throw through ensure execution failed");
  expect(ensure_result.value.is_integer() &&
             ensure_result.value.as_integer() == 19,
         "ensure should run before matching catch resumes");

  amber::runtime::ExecutionResult unhandled =
      execute_emitted_init("throw :missing, 1\n");
  expect(!unhandled.ok(), "unhandled throw should fail");
  expect(unhandled.fault.has_value() &&
             unhandled.fault->error_name == "UncaughtThrowError",
         "unhandled throw should report UncaughtThrowError");
}

void test_manual_ensure_suppresses_pending_exception() {
  using namespace amber::bytecode;

  BcModule module;
  module.symbols = {"Primary", "Cleanup"};

  BcClass primary_class;
  primary_class.class_name_sym_id = 0;
  module.classes.push_back(primary_class);
  BcClass cleanup_class;
  cleanup_class.class_name_sym_id = 1;
  module.classes.push_back(cleanup_class);

  BcCode code;
  code.code_id = 1;
  code.kind = CodeKind::Method;
  code.reg_count = 2;
  code.instructions.push_back({Opcode::Raise, {{0, false}}});
  code.instructions.push_back({Opcode::LoadNull, {{0, false}}});
  code.instructions.push_back({Opcode::Return, {{0, false}}});
  code.handler_table.push_back(
      {0, 1, 1, 2, handler_flags(kHandlerKindEnsure, 0, 0)});

  BcCode ensure;
  ensure.code_id = 2;
  ensure.kind = CodeKind::Ensure;
  ensure.reg_count = 2;
  ensure.instructions.push_back({Opcode::Raise, {{1, false}}});
  ensure.instructions.push_back({Opcode::LoadNull, {{0, false}}});
  ensure.instructions.push_back({Opcode::Return, {{0, false}}});
  module.code_objects = {code, ensure};

  auto primary =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  primary->class_index = 0;
  auto cleanup =
      amber::runtime::make_intrusive<amber::runtime::InstanceValue>();
  cleanup->class_index = 1;
  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(module, 1,
                                   {amber::runtime::Value::instance(primary),
                                    amber::runtime::Value::instance(cleanup)});
  expect(!exec.ok() && exec.fault.has_value() &&
             exec.fault->error_name == "Cleanup",
         "cleanup exception should replace pending exception");

  const auto suppressed = cleanup->ivars.find("suppressed_exceptions");
  expect(suppressed != cleanup->ivars.end() && suppressed->second.is_list(),
         "cleanup exception should keep suppressed exceptions");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue>
      suppressed_list = suppressed->second.as_list();
  expect(suppressed_list != nullptr && suppressed_list->items.size() == 1,
         "one pending exception should be suppressed");
  expect(suppressed_list->items[0].is_instance_object() &&
             suppressed_list->items[0].as_instance_object() == primary,
         "suppressed exception preserves original pending exception");
}

void test_runtime_io_v2_source_surface() {
  amber::runtime::ExecutionResult buffer = execute_emitted_init(
      "buf = io.ByteBuffer(4)\n"
      "buf.put!(65)\n"
      "buf.put!(66)\n"
      "buf.flip!()\n"
      "[buf.get!(), buf.remaining(), buf.bytes().hex()]\n");
  expect(buffer.ok(),
         "ByteBuffer v2 source execution failed: " +
             (buffer.fault.has_value()
                  ? buffer.fault->error_name + " " + buffer.fault->message
                  : std::string{}));
  expect(buffer.value.is_list(), "ByteBuffer source result should be Array");
  const auto buffer_items = buffer.value.as_list()->items;
  expect(
      buffer_items.size() == 3 && buffer_items[0].is_integer() &&
          buffer_items[0].as_integer() == 65 && buffer_items[1].is_integer() &&
          buffer_items[1].as_integer() == 1 && buffer_items[2].is_string() &&
          buffer_items[2].as_string().string_id <
              buffer.runtime_strings.size() &&
          buffer.runtime_strings[buffer_items[2].as_string().string_id] == "42",
      "ByteBuffer v2 source contract mismatch");

  amber::runtime::ExecutionResult path =
      execute_emitted_init("p = fs.Path(\"/tmp\") / \"amber.txt\"\n"
                           "[p.basename(), p.extname(), p.absolute?()]\n");
  expect(path.ok(), "Path v2 source execution failed");
  expect(path.value.is_list(), "Path source result should be Array");
  const auto path_items = path.value.as_list()->items;
  expect(path_items.size() == 3 && path_items[0].is_string() &&
             path_items[1].is_string() && path_items[2].is_bool() &&
             path_items[2].as_bool(),
         "Path v2 source contract mismatch");

  amber::runtime::ExecutionResult endpoint =
      execute_emitted_init("a = net.Endpoint.new(\"localhost\", 80)\n"
                           "b = net.Endpoint.parse(\"127.0.0.1:8080\")\n"
                           "types = [net.tcp, net.udp, net.http]\n"
                           "http_types = [net.http.RequestBody, "
                           "net.http.ServerRequest, net.http.ServerResponse, "
                           "net.http.json, net.http.form]\n"
                           "[a.host(), a.port(), b.to_str(), types.count, "
                           "http_types.count]\n");
  expect(endpoint.ok(),
         "Endpoint source execution failed: " +
             (endpoint.fault.has_value()
                  ? endpoint.fault->error_name + " " + endpoint.fault->message
                  : std::string{}));
  expect(endpoint.value.is_list(), "Endpoint source result should be Array");
  const auto endpoint_items = endpoint.value.as_list()->items;
  expect(endpoint_items.size() == 5 && endpoint_items[0].is_string() &&
             endpoint_items[1].is_integer() &&
             endpoint_items[1].as_integer() == 80 &&
             endpoint_items[2].is_string() && endpoint_items[3].is_integer() &&
             endpoint_items[3].as_integer() == 3 &&
             endpoint_items[4].is_integer() &&
             endpoint_items[4].as_integer() == 5,
         "Endpoint source contract mismatch");

  amber::runtime::ExecutionResult udp_family =
      execute_emitted_init("net.udp.open(family: :unix)\n");
  expect(!udp_family.ok() && udp_family.fault.has_value() &&
             udp_family.fault->error_name == "ArgumentError",
         "UDP open source validation should reject unsupported family");
  amber::runtime::ExecutionResult udp_endpoint =
      execute_emitted_init("net.udp.bind(\"127.0.0.1\", 70000)\n");
  expect(!udp_endpoint.ok() && udp_endpoint.fault.has_value() &&
             udp_endpoint.fault->error_name == "ArgumentError",
         "UDP bind source validation should reject invalid endpoint");
  amber::runtime::ExecutionResult tcp_timeout =
      execute_emitted_init("endpoint = net.Endpoint.new(\"127.0.0.1\", 1)\n"
                           "net.tcp.connect(endpoint, timeout: -1)\n");
  expect(!tcp_timeout.ok() && tcp_timeout.fault.has_value() &&
             tcp_timeout.fault->error_name == "ArgumentError",
         "TCP connect source validation should reject invalid timeout");
  amber::runtime::ExecutionResult tcp_backlog =
      execute_emitted_init("net.tcp.listen(\"127.0.0.1\", 0, backlog: 0)\n");
  expect(!tcp_backlog.ok() && tcp_backlog.fault.has_value() &&
             tcp_backlog.fault->error_name == "ArgumentError",
         "TCP listen source validation should reject invalid backlog");

  amber::runtime::ExecutionResult pipe =
      execute_emitted_init("pair = io.Pipe.new(capacity: 2)\n"
                           "r = pair[0]\n"
                           "w = pair[1]\n"
                           "a = w.try_write!(\"abc\")\n"
                           "buf = io.ByteBuffer(2)\n"
                           "b = r.read!(buf)\n"
                           "w.close!()\n"
                           "buf.clear!()\n"
                           "c = r.read!(buf)\n"
                           "buf.clear!()\n"
                           "d = r.read!(buf)\n"
                           "[a, b, c, d]\n");
  expect(pipe.ok(), "Pipe v2 source execution failed");
  expect(pipe.value.is_list(), "Pipe source result should be Array");
  const auto pipe_items = pipe.value.as_list()->items;
  expect(pipe_items.size() == 4 && pipe_items[0].as_integer() == 2 &&
             pipe_items[1].as_integer() == 2 &&
             pipe_items[2].as_integer() == 0 && pipe_items[3].as_integer() == 0,
         "Pipe v2 source contract mismatch");

  const std::string file_path =
      "/tmp/amber_vm_io_" + std::to_string(::getpid()) + ".txt";
  amber::runtime::ExecutionResult file =
      execute_emitted_init("path = \"" + file_path +
                           "\"\n"
                           "fs.write_bytes(path, \"abc\")\n"
                           "fs.write_text(path, \"hello\")\n"
                           "meta = fs.metadata(path)\n"
                           "fs.File.open(path, :read) |f|:\n"
                           "  [fs.exists?(path), fs.file?(path), "
                           "fs.dir?(path), meta.file?(), meta.dir?(), "
                           "meta.size(), fs.read_text(path), "
                           "fs.read_bytes(path, limit: 5).to_str(), "
                           "f.size!(), f.read_all!().to_str(), "
                           "f.path().to_str() == path, f.mode() == :read, "
                           "f.closed?(), f.close!(), f.closed?()]\n");
  expect(file.ok(), "File v2 source execution failed");
  expect(file.value.is_list(), "File source result should be Array");
  const auto file_items = file.value.as_list()->items;
  expect(file_items.size() == 15, "File v2 source result shape mismatch");
  expect(file_items[0].is_bool() && file_items[0].as_bool() &&
             file_items[1].is_bool() && file_items[1].as_bool() &&
             file_items[2].is_bool() && !file_items[2].as_bool() &&
             file_items[3].is_bool() && file_items[3].as_bool() &&
             file_items[4].is_bool() && !file_items[4].as_bool(),
         "File v2 source filesystem predicates mismatch");
  expect(file_items[5].is_integer() && file_items[5].as_integer() == 5 &&
             file_items[6].is_string() && file_items[7].is_string() &&
             file_items[8].is_integer() && file_items[8].as_integer() == 5 &&
             file_items[9].is_string(),
         "File v2 source read/metadata mismatch");
  expect(file_items[10].is_bool() && file_items[10].as_bool(),
         "File v2 source fs.File.path mismatch");
  expect(file_items[11].is_bool() && file_items[11].as_bool(),
         "File v2 source fs.File.mode mismatch");
  expect(file_items[12].is_bool() && !file_items[12].as_bool() &&
             file_items[13].is_null() && file_items[14].is_bool() &&
             file_items[14].as_bool(),
         "File v2 source fs.File lifecycle mismatch");

  const std::string copy_path = file_path + ".copy";
  const std::string renamed_path = file_path + ".renamed";
  ::unlink(copy_path.c_str());
  ::unlink(renamed_path.c_str());
  amber::runtime::ExecutionResult moves =
      execute_emitted_init("source = \"" + file_path +
                           "\"\n"
                           "copy = \"" +
                           copy_path +
                           "\"\n"
                           "renamed = \"" +
                           renamed_path +
                           "\"\n"
                           "count = fs.copy(source, copy)\n"
                           "fs.rename(copy, renamed)\n"
                           "text = fs.read_text(renamed)\n"
                           "fs.remove(renamed)\n"
                           "[count, text, fs.exists?(renamed)]\n");
  expect(moves.ok(), "File move source execution failed");
  expect(moves.value.is_list(), "File move result should be Array");
  const auto move_items = moves.value.as_list()->items;
  expect(move_items.size() == 3 && move_items[0].is_integer() &&
             move_items[0].as_integer() == 5 && move_items[1].is_string() &&
             move_items[2].is_bool() && !move_items[2].as_bool(),
         "File move source contract mismatch");
  ::unlink(copy_path.c_str());
  ::unlink(renamed_path.c_str());
  ::unlink(file_path.c_str());

  const std::string dir_path =
      "/tmp/amber_vm_io_dir_" + std::to_string(::getpid());
  const std::string parent_path = dir_path + "/a";
  const std::string nested_path = parent_path + "/b";
  ::rmdir(nested_path.c_str());
  ::rmdir(parent_path.c_str());
  ::rmdir(dir_path.c_str());
  amber::runtime::ExecutionResult dirs =
      execute_emitted_init("root = \"" + dir_path +
                           "\"\n"
                           "parent = \"" +
                           parent_path +
                           "\"\n"
                           "nested = \"" +
                           nested_path +
                           "\"\n"
                           "fs.mkdir(root)\n"
                           "fs.mkdir_p(nested)\n"
                           "a = fs.dir?(root)\n"
                           "b = fs.dir?(nested)\n"
                           "fs.remove(nested)\n"
                           "fs.remove(parent)\n"
                           "fs.remove(root)\n"
                           "[a, b, fs.exists?(root)]\n");
  expect(dirs.ok(), "Directory v2 source execution failed");
  expect(dirs.value.is_list(), "Directory source result should be Array");
  const auto dir_items = dirs.value.as_list()->items;
  expect(dir_items.size() == 3 && dir_items[0].is_bool() &&
             dir_items[0].as_bool() && dir_items[1].is_bool() &&
             dir_items[1].as_bool() && dir_items[2].is_bool() &&
             !dir_items[2].as_bool(),
         "Directory source contract mismatch");
  ::rmdir(nested_path.c_str());
  ::rmdir(parent_path.c_str());
  ::rmdir(dir_path.c_str());
}

void test_runtime_io_v2_policy_and_replay() {
  const std::string file_path =
      "/tmp/amber_vm_io_policy_" + std::to_string(::getpid()) + ".txt";
  {
    std::ofstream output(file_path, std::ios::binary | std::ios::trunc);
    output << "policy";
  }

  amber::bytecode::EmitResult emitted =
      emit_ok("fs.read_bytes(\"" + file_path + "\").count()\n");
  emitted.module.capabilities.push_back(
      amber::capability::make_capability("fs.read", file_path));

  amber::runtime::RuntimeWorldOptions allowed_options;
  allowed_options.enforce_effects = true;
  allowed_options.allowed_effects = {"fs"};
  allowed_options.capability_grants.push_back(
      amber::capability::make_capability("fs.read", file_path));
  allowed_options.record_replay_trace = true;
  amber::runtime::RuntimeWorld allowed_world(emitted.module, allowed_options);
  amber::runtime::ExecutionResult allowed =
      allowed_world.execute(emitted.module.init.entry_code_id);
  expect(allowed.ok() && allowed.value.is_integer() &&
             allowed.value.as_integer() == 6,
         "RuntimeWorld should allow scoped file IO");
  const amber::runtime::RuntimeReplayTrace trace = allowed_world.replay_trace();
  bool saw_io_wait = false;
  for (const amber::runtime::RuntimeTraceEvent &event : trace.events) {
    saw_io_wait = saw_io_wait || event.name == "io.wait";
  }
  expect(saw_io_wait, "file IO should emit io.wait trace metadata");

  amber::runtime::RuntimeWorldOptions effect_options = allowed_options;
  effect_options.record_replay_trace = false;
  effect_options.allowed_effects = {"net"};
  amber::runtime::RuntimeWorld effect_world(emitted.module, effect_options);
  amber::runtime::ExecutionResult effect_denied =
      effect_world.execute(emitted.module.init.entry_code_id);
  expect(!effect_denied.ok() && effect_denied.fault.has_value() &&
             effect_denied.fault->error_name == "EffectViolationError",
         "IO effect denial should precede capability and host access");

  amber::runtime::RuntimeWorldOptions capability_options = allowed_options;
  capability_options.record_replay_trace = false;
  capability_options.capability_grants.clear();
  amber::runtime::RuntimeWorld capability_world(emitted.module,
                                                capability_options);
  amber::runtime::ExecutionResult capability_denied =
      capability_world.execute(emitted.module.init.entry_code_id);
  expect(!capability_denied.ok() && capability_denied.fault.has_value() &&
             capability_denied.fault->error_name == "CapabilityError",
         "missing file capability should fail before host access");

  amber::bytecode::EmitResult invalid = emit_ok("fs.read_bytes(1)\n");
  amber::runtime::RuntimeWorld invalid_world(invalid.module, effect_options);
  amber::runtime::ExecutionResult invalid_result =
      invalid_world.execute(invalid.module.init.entry_code_id);
  expect(!invalid_result.ok() && invalid_result.fault.has_value() &&
             invalid_result.fault->error_name == "TypeError",
         "local IO argument errors should precede policy errors");

  amber::runtime::RuntimeWorldOptions replay_options = allowed_options;
  replay_options.record_replay_trace = false;
  replay_options.enforce_replay = true;
  amber::runtime::RuntimeWorld replay_world(emitted.module, replay_options);
  amber::runtime::ExecutionResult replay_denied =
      replay_world.execute(emitted.module.init.entry_code_id);
  expect(!replay_denied.ok() && replay_denied.fault.has_value() &&
             replay_denied.fault->error_name == "ReplayProviderError",
         "replay mode should reject direct host file IO without provider");

  ::unlink(file_path.c_str());
}

void test_runtime_io_v2_replay_provider_file_surface() {
  amber::bytecode::EmitResult emitted = emit_ok(
      "fs.File.open(\"out.txt\", :write, create: true, truncate: true) |f|:\n"
      "  f.write_all!(\"abc\")\n"
      "fs.read_text(\"out.txt\")\n");
  emitted.module.capabilities.push_back(
      amber::capability::make_capability("fs.write", "out.txt"));
  emitted.module.capabilities.push_back(
      amber::capability::make_capability("fs.read", "out.txt"));

  auto provider = std::make_shared<TestIoProvider>();
  amber::runtime::RuntimeWorldOptions options;
  options.enforce_effects = true;
  options.allowed_effects = {"fs"};
  options.enforce_replay = true;
  options.record_replay_trace = true;
  options.io_provider = provider;
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.write", "out.txt"));
  options.capability_grants.push_back(
      amber::capability::make_capability("fs.read", "out.txt"));

  amber::runtime::RuntimeWorld world(emitted.module, options);
  amber::runtime::ExecutionResult result =
      world.execute(emitted.module.init.entry_code_id);
  expect(result.ok(),
         "provider-backed fs.File.open should execute in replay mode: " +
             (result.fault.has_value()
                  ? result.fault->error_name + " " + result.fault->message
                  : std::string{}));
  expect(provider->files["out.txt"] == "abc",
         "provider-backed file close should commit written bytes");
  const std::string provider_text =
      string_value_text_or_die(result.value, emitted.module, result);
  expect(provider_text == "abc",
         "provider-backed fs.read_text should return committed bytes, got " +
             provider_text);
}

void test_runtime_io_v2_low_level_wait_trace() {
  amber::bytecode::EmitResult emitted =
      emit_ok("pair = io.Pipe.new(capacity: 1)\n"
              "r = pair[0]\n"
              "w = pair[1]\n"
              "w.write!(\"x\")\n"
              "buf = io.ByteBuffer(1)\n"
              "r.read!(buf)\n");

  amber::runtime::RuntimeWorldOptions options;
  options.record_replay_trace = true;
  amber::runtime::RuntimeWorld world(emitted.module, options);
  amber::runtime::ExecutionResult result =
      world.execute(emitted.module.init.entry_code_id);
  expect(result.ok(), "pipe read trace scenario should execute");

  bool saw_enter = false;
  bool saw_exit = false;
  for (const amber::runtime::RuntimeTraceEvent &event :
       world.replay_trace().events) {
    if (event.name != "io.wait") {
      continue;
    }
    bool pipe_read = false;
    bool enter = false;
    bool exit = false;
    for (const amber::replay::TraceAttribute &attribute : event.attributes) {
      if (attribute.key == "operation" && attribute.value == "pipe read") {
        pipe_read = true;
      }
      if (attribute.key == "phase" && attribute.value == "enter") {
        enter = true;
      }
      if (attribute.key == "phase" && attribute.value == "exit") {
        exit = true;
      }
    }
    saw_enter = saw_enter || (pipe_read && enter);
    saw_exit = saw_exit || (pipe_read && exit);
  }
  expect(saw_enter && saw_exit,
         "low-level pipe read should emit io.wait enter and exit events");
}

void test_foreign_handle_lifetime() {
  using amber::runtime::RuntimeForeignHandle;
  using Ownership = RuntimeForeignHandle::Ownership;

  // owned: deterministic destroy! runs teardown exactly once with the supplied
  // ctx; the GC never does.
  {
    int freed = 0;
    void *seen_ctx = nullptr;
    int ctx_marker = 7;
    auto handle = std::make_shared<RuntimeForeignHandle>();
    handle->tag = "pkg.Owned";
    handle->ownership = Ownership::Owned;
    handle->teardown = [&freed, &seen_ctx](void *ctx, void *) {
      ++freed;
      seen_ctx = ctx;
    };
    expect(handle->destroy(&ctx_marker),
           "owned destroy! reports it destroyed the handle");
    expect(freed == 1, "owned destroy! runs teardown once");
    expect(seen_ctx == &ctx_marker,
           "owned teardown receives the destroy!-time ctx");
    expect(!handle->destroy(&ctx_marker), "second destroy! is a no-op");
    expect(freed == 1, "owned teardown does not run twice");
    handle.reset();
    expect(freed == 1, "destroyed owned handle does not re-run teardown on GC");
  }
  // owned, never destroyed: the GC must NOT run the destructor.
  {
    int freed = 0;
    {
      auto handle = std::make_shared<RuntimeForeignHandle>();
      handle->ownership = Ownership::Owned;
      handle->teardown = [&freed](void *, void *) { ++freed; };
    }
    expect(freed == 0,
           "GC never runs an owned destructor (no implicit finalizer)");
  }
  // collected, never destroyed: the GC reclaims it with a null ctx.
  {
    int freed = 0;
    bool null_ctx = false;
    {
      auto handle = std::make_shared<RuntimeForeignHandle>();
      handle->ownership = Ownership::Collected;
      handle->teardown = [&freed, &null_ctx](void *ctx, void *) {
        ++freed;
        null_ctx = ctx == nullptr;
      };
    }
    expect(freed == 1, "collected handle is reclaimed by the GC");
    expect(null_ctx, "GC reclaim passes a null ctx (context-free reclaim)");
  }
  // collected, explicitly destroyed: teardown runs once, not again on GC.
  {
    int freed = 0;
    auto handle = std::make_shared<RuntimeForeignHandle>();
    handle->ownership = Ownership::Collected;
    handle->teardown = [&freed](void *, void *) { ++freed; };
    expect(handle->destroy() && freed == 1, "collected destroy! runs teardown");
    handle.reset();
    expect(freed == 1, "collected handle is not double-freed after destroy!");
  }
  // borrowed: never freed, whether via destroy! or GC.
  {
    int freed = 0;
    {
      auto handle = std::make_shared<RuntimeForeignHandle>();
      handle->ownership = Ownership::Borrowed;
      handle->teardown = [&freed](void *, void *) { ++freed; };
      expect(handle->destroy(), "borrowed destroy! flips the tombstone");
    }
    expect(freed == 0, "borrowed handle is never freed by Amber");
  }
  // The handle round-trips as a Value tail kind (exercised on both reps).
  {
    auto handle = std::make_shared<RuntimeForeignHandle>();
    handle->tag = "pkg.Box";
    handle->ownership = Ownership::Borrowed;
    amber::runtime::Value value = amber::runtime::Value::foreign_handle(handle);
    expect(value.is_foreign_handle(),
           "Value::foreign_handle round-trips as a handle kind");
    expect(!value.is_null() && !value.is_uuid(),
           "a foreign handle Value is distinct from other kinds");
    expect(value.as_foreign_handle() == handle,
           "as_foreign_handle recovers the same underlying object");
    amber::runtime::Value copy = value;
    expect(copy.is_foreign_handle() && copy.as_foreign_handle() == handle,
           "copying a foreign handle Value shares the underlying handle");
  }
}

void test_native_tag_registry() {
  using amber::runtime::NativeTagRegistry;
  using amber::runtime::NativeTypeDescriptor;
  using Ownership = amber::runtime::RuntimeForeignHandle::Ownership;

  NativeTagRegistry registry;
  expect(registry.lookup("absent") == nullptr,
         "an unknown tag resolves to nullptr");

  NativeTypeDescriptor owned;
  owned.tag = "pkg.Owned";
  owned.ownership = Ownership::Owned;
  owned.owned_destructor = [](void *, void *) {};
  registry.register_type(owned);

  NativeTypeDescriptor collected;
  collected.tag = "pkg.Collected";
  collected.ownership = Ownership::Collected;
  collected.collected_reclaim = [](void *) {};
  registry.register_type(collected);

  expect(registry.size() == 2, "registry holds both descriptors");
  const NativeTypeDescriptor *owned_lookup = registry.lookup("pkg.Owned");
  expect(owned_lookup != nullptr &&
             owned_lookup->ownership == Ownership::Owned &&
             owned_lookup->owned_destructor != nullptr &&
             owned_lookup->collected_reclaim == nullptr,
         "owned descriptor resolves with a destructor and no reclaim");
  const NativeTypeDescriptor *collected_lookup =
      registry.lookup("pkg.Collected");
  expect(collected_lookup != nullptr &&
             collected_lookup->ownership == Ownership::Collected &&
             collected_lookup->collected_reclaim != nullptr &&
             collected_lookup->owned_destructor == nullptr,
         "collected descriptor resolves with a reclaim and no destructor");
}

// M1: the first-class `Ast` value model. Parse a snippet, wrap a subnode as an
// Ast value, and confirm the runtime tail kind + accessors round-trip the node
// kind and return the verbatim source slice for the node's span.
void test_ast_value_model() {
  const std::string source = "x > 5\n";
  amber::lexer::Lexer lexer(source, "<ast-test>");
  amber::lexer::LexResult lex = lexer.lex();
  expect(lex.ok(), "ast value: lex ok");
  amber::parser::Parser parser(lex.tokens);
  amber::parser::ParseModuleResult mod = parser.parse_module_unit();
  expect(mod.ok() && mod.items.size() == 1, "ast value: parse ok");

  const amber::ast::Expr *expr = nullptr;
  for (const auto &nf : mod.items[0]->node_fields) {
    if (nf.name == "expr") {
      expr = nf.value.get();
    }
  }
  expect(expr != nullptr, "ast value: found statement expr");

  auto root = std::shared_ptr<const amber::ast::Expr>(std::move(mod.items[0]));
  auto src = std::make_shared<const std::string>(source);
  auto node = std::make_shared<amber::runtime::RuntimeAstNode>();
  node->root = root;
  node->node = expr; // aliasing pointer into the root-owned tree
  node->source = src;
  amber::runtime::Value value = amber::runtime::Value::ast_node(node);

  expect(value.is_ast_node(), "value is an ast_node");
  expect(!value.is_integer() && !value.is_string() && !value.is_list(),
         "ast_node is distinct from scalar/collection kinds");
  const std::shared_ptr<amber::runtime::RuntimeAstNode> got = value.as_ast_node();
  expect(got != nullptr && got->node == expr,
         "as_ast_node round-trips the aliasing node pointer");
  expect(!amber::runtime::runtime_ast_node_kind(*got).empty() &&
             amber::runtime::runtime_ast_node_kind(*got) == expr->kind,
         "runtime_ast_node_kind reports the node's amber.ast.v1 kind");
  expect(amber::runtime::runtime_ast_node_source(*got) == "x > 5",
         "runtime_ast_node_source returns the verbatim span slice");

  // Copy/move must preserve the payload (exercises both Value reps' refcounting).
  amber::runtime::Value copy = value;
  expect(copy.is_ast_node() && copy.as_ast_node()->node == expr,
         "ast_node survives value copy");
}

// Run F1.5 expansion over a source module and return the result.
amber::macros::ExpandResult expand_source(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<macro-sandbox-test>");
  amber::lexer::LexResult lex = lexer.lex();
  expect(lex.ok(), "macro sandbox: lex ok");
  amber::parser::Parser parser(lex.tokens);
  amber::parser::ParseModuleResult mod = parser.parse_module_unit();
  expect(mod.ok(), "macro sandbox: parse ok");
  return amber::macros::expand_macros(mod.items, "sandbox.test", source);
}

void test_macro_expander_sandbox() {
  // Positive control: an ordinary macro expands under the sandbox.
  {
    const amber::macros::ExpandResult ok = expand_source(
        "macro def double(x):\n"
        "  quote:\n"
        "    unquote(x) + unquote(x)\n"
        "\n"
        "double(21)\n");
    expect(ok.ok, "sandboxed expansion still succeeds for pure macros");
  }

  // A macro attempting IO at expansion time is a build diagnostic: the
  // expander VM runs with the capability gate armed and nothing granted
  // (DESIGN-macro-system §10).
  {
    const amber::macros::ExpandResult denied = expand_source(
        "macro def sneaky(x):\n"
        "  data = fs.read_text(\"/etc/hosts\")\n"
        "  return quote:\n"
        "    1\n"
        "\n"
        "sneaky(1)\n");
    expect(!denied.ok, "macro IO is rejected");
    expect(denied.error.find("AMB_MACRO_CAPABILITY") != std::string::npos,
           "macro IO reports AMB_MACRO_CAPABILITY, got: " + denied.error);
  }

  // A non-terminating macro exhausts the step budget instead of hanging the
  // build; the budget is shared with nested block VMs via RuntimeState.
  {
    const amber::macros::ExpandResult spun = expand_source(
        "macro def spin(x):\n"
        "  n = 0\n"
        "  while true:\n"
        "    n = n + 1\n"
        "  return quote:\n"
        "    1\n"
        "\n"
        "spin(1)\n");
    expect(!spun.ok, "looping macro is rejected");
    expect(spun.error.find("AMB_MACRO_BUDGET") != std::string::npos,
           "looping macro reports AMB_MACRO_BUDGET, got: " + spun.error);
  }
}

void test_cross_module_macro_staging() {
  // Provider module: exports a macro through the macro-marked export table
  // (DESIGN-macro-system §11 exports/imports).
  const std::string provider_source = "package provider.mod\n"
                                      "export macro run_twice\n"
                                      "\n"
                                      "macro def run_twice(blk):\n"
                                      "  #{blk}\n"
                                      "  #{blk}\n"
                                      "\n"
                                      "macro def private_macro(x):\n"
                                      "  #{x}\n";
  amber::lexer::Lexer provider_lexer(provider_source, "<provider>");
  amber::lexer::LexResult provider_lex = provider_lexer.lex();
  expect(provider_lex.ok(), "staging: provider lex ok");
  amber::parser::Parser provider_parser(provider_lex.tokens);
  amber::parser::ParseModuleResult provider_mod =
      provider_parser.parse_module_unit();
  expect(provider_mod.ok(), "staging: provider parse ok");
  std::vector<amber::macros::MacroExport> exports =
      amber::macros::collect_macro_exports(provider_mod.items);
  expect(exports.size() == 1 && exports[0].public_name == "run_twice",
         "only the macro-marked export is harvested (private stays private)");

  amber::macros::MacroProviderMap providers;
  providers["provider.mod"] = std::move(exports);

  // Importer: binds the provider macro under a local alias and invokes it
  // through the paren-less block trigger, which only a macro can consume.
  const std::string importer_source = "package app.main\n"
                                      "from provider.mod import run_twice as loop2\n"
                                      "\n"
                                      "def probe():\n"
                                      "  y = 0\n"
                                      "  loop2:\n"
                                      "    y = y + 21\n"
                                      "  y\n";
  amber::lexer::Lexer importer_lexer(importer_source, "<importer>");
  amber::lexer::LexResult importer_lex = importer_lexer.lex();
  expect(importer_lex.ok(), "staging: importer lex ok");
  amber::parser::Parser importer_parser(importer_lex.tokens);
  amber::parser::ParseModuleResult importer_mod =
      importer_parser.parse_module_unit();
  expect(importer_mod.ok(), "staging: importer parse ok");

  const amber::macros::ExpandResult expanded = amber::macros::expand_macros(
      importer_mod.items, importer_mod.module_name, importer_source, providers);
  expect(expanded.ok,
         "cross-module expansion succeeds, got: " + expanded.error);

  // Compile and run the expanded importer: the imported macro spliced the
  // block twice, so probe() == 42.
  amber::ast::expand_quotes(importer_mod.items);
  amber::binder::BindResult bind =
      amber::binder::bind_module(importer_mod.items, importer_mod.module_name);
  expect(bind.ok(), "staging: expanded importer binds");
  amber::hir::Program program = amber::hir::lower_module(
      importer_mod.items, importer_mod.module_name, bind.graph);
  amber::bytecode::EmitResult emit =
      amber::bytecode::emit_program(program, importer_mod.module_name);
  expect(emit.ok(), "staging: expanded importer emits");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit.module, "probe");
  expect(probe != nullptr, "staging: probe method exists");
  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit.module, probe->entry_code_id);
  expect(exec.ok(), "staging: probe executes");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "imported macro expanded at the call site (probe == 42)");

  // A local macro def colliding with an imported alias is a diagnostic.
  const std::string collision_source = "package app.other\n"
                                       "from provider.mod import run_twice\n"
                                       "\n"
                                       "macro def run_twice(x):\n"
                                       "  #{x}\n"
                                       "\n"
                                       "def probe():\n"
                                       "  run_twice(1)\n";
  amber::lexer::Lexer collision_lexer(collision_source, "<collision>");
  amber::lexer::LexResult collision_lex = collision_lexer.lex();
  amber::parser::Parser collision_parser(collision_lex.tokens);
  amber::parser::ParseModuleResult collision_mod =
      collision_parser.parse_module_unit();
  expect(collision_mod.ok(), "staging: collision module parses");
  const amber::macros::ExpandResult collided = amber::macros::expand_macros(
      collision_mod.items, collision_mod.module_name, collision_source,
      providers);
  expect(!collided.ok &&
             collided.error.find("collides") != std::string::npos,
         "import/local macro name collision is a diagnostic");
}

void test_string_tag_macro_staging() {
  // Provider exports a string_tag macro (§8.5); the importer invokes it under
  // an alias through the tag surface — the sql""" dialect-selection pattern
  // from the multiline text-block design §7.
  const std::string provider_source =
      "package db.mock\n"
      "export macro sql\n"
      "\n"
      "string_tag macro def sql(t as Ast.StringTemplate):\n"
      "  query = \"\"\n"
      "  holes = []\n"
      "  t.parts.each |part|:\n"
      "    if part.kind == \"AstStringText\":\n"
      "      query = query + part.value\n"
      "    else:\n"
      "      query = query + \"?\"\n"
      "      holes.push!(part.expr)\n"
      "  return Ast.node(\"AstTupleLiteral\", {\n"
      "    elements: [\n"
      "      Ast.lift(query),\n"
      "      Ast.node(\"AstListLiteral\", {elements: holes})\n"
      "    ]\n"
      "  })\n";
  amber::lexer::Lexer provider_lexer(provider_source, "<tag-provider>");
  amber::lexer::LexResult provider_lex = provider_lexer.lex();
  expect(provider_lex.ok(), "tag staging: provider lex ok");
  amber::parser::Parser provider_parser(provider_lex.tokens);
  amber::parser::ParseModuleResult provider_mod =
      provider_parser.parse_module_unit();
  expect(provider_mod.ok(), "tag staging: provider parse ok");
  amber::macros::MacroProviderMap providers;
  providers["db.mock"] =
      amber::macros::collect_macro_exports(provider_mod.items);
  expect(providers["db.mock"].size() == 1,
         "tag staging: string_tag export harvested");

  const std::string importer_source = "package app.q\n"
                                      "from db.mock import sql as q\n"
                                      "\n"
                                      "def probe():\n"
                                      "  pair = q\"\"\"\n"
                                      "    A #{40 + 2} B\n"
                                      "    \"\"\"\n"
                                      "  pair[1][0]\n";
  amber::lexer::Lexer importer_lexer(importer_source, "<tag-importer>");
  amber::lexer::LexResult importer_lex = importer_lexer.lex();
  expect(importer_lex.ok(), "tag staging: importer lex ok");
  amber::parser::Parser importer_parser(importer_lex.tokens);
  amber::parser::ParseModuleResult importer_mod =
      importer_parser.parse_module_unit();
  expect(importer_mod.ok(), "tag staging: importer parse ok");
  const amber::macros::ExpandResult expanded = amber::macros::expand_macros(
      importer_mod.items, importer_mod.module_name, importer_source,
      providers);
  expect(expanded.ok,
         "imported string_tag expansion succeeds, got: " + expanded.error);
  amber::ast::expand_quotes(importer_mod.items);
  amber::binder::BindResult bind =
      amber::binder::bind_module(importer_mod.items, importer_mod.module_name);
  expect(bind.ok(), "tag staging: expanded importer binds");
  amber::hir::Program program = amber::hir::lower_module(
      importer_mod.items, importer_mod.module_name, bind.graph);
  amber::bytecode::EmitResult emit =
      amber::bytecode::emit_program(program, importer_mod.module_name);
  expect(emit.ok(), "tag staging: expanded importer emits");
  const amber::bytecode::BcMethod *probe =
      method_by_name(emit.module, "probe");
  expect(probe != nullptr, "tag staging: probe method exists");
  const amber::runtime::ExecutionResult exec =
      amber::runtime::execute_code(emit.module, probe->entry_code_id);
  expect(exec.ok(), "tag staging: probe executes");
  expect(exec.value.is_integer() && exec.value.as_integer() == 42,
         "imported tag bound the interpolant as a parameter (probe == 42)");

  // Misusing a string_tag macro through an ordinary call channel is a
  // located diagnostic, not a wrong-channel expansion.
  const std::string misuse_source = "package app.misuse\n"
                                    "from db.mock import sql as q\n"
                                    "\n"
                                    "def probe():\n"
                                    "  q(1)\n";
  amber::lexer::Lexer misuse_lexer(misuse_source, "<tag-misuse>");
  amber::lexer::LexResult misuse_lex = misuse_lexer.lex();
  amber::parser::Parser misuse_parser(misuse_lex.tokens);
  amber::parser::ParseModuleResult misuse_mod =
      misuse_parser.parse_module_unit();
  expect(misuse_mod.ok(), "tag staging: misuse module parses");
  const amber::macros::ExpandResult misused = amber::macros::expand_macros(
      misuse_mod.items, misuse_mod.module_name, misuse_source, providers);
  expect(!misused.ok &&
             misused.error.find("string_tag") != std::string::npos,
         "ordinary call of a string_tag macro is a diagnostic, got: " +
             misused.error);
}

} // namespace

int main() {
  test_ast_value_model();
  test_macro_expander_sandbox();
  test_cross_module_macro_staging();
  test_string_tag_macro_staging();
  test_execute_emitted_method();
  test_direct_entry_materializes_sibling_captures();
  test_execute_return_keyword();
  test_execute_return_runs_nested_ensure_inner_to_outer();
  test_execute_zero_division_error();
  test_execute_checked_int_overflow();
  test_execute_numeric_profile_modes();
  test_execute_big_int_explicit_type();
  test_execute_mixed_sign_division_semantics();
  test_execute_module_init_calls_top_level_def();
  test_execute_native_range_literal();
  test_execute_emitted_collection_literals();
  test_execute_emitted_v20_7_spread();
  test_execute_emitted_v20_6_value_keyed_maps();
  test_runtime_string_interpolation_and_conversions();
  test_runtime_text_output_helpers_and_io_sinks();
  test_runtime_logger_source_surface_and_annotations();
  test_runtime_logger_parallel_native_threads_and_tasks();
  test_runtime_io_v2_source_surface();
  test_runtime_io_v2_policy_and_replay();
  test_runtime_io_v2_replay_provider_file_surface();
  test_runtime_io_v2_low_level_wait_trace();
  test_top_level_function_closure_captures_sibling_function();
  test_direct_top_level_method_entry_materializes_module_captures();
  test_top_level_function_self_recursion();
  test_top_level_clause_function_self_recursion();
  test_top_level_plain_def_fallback_clause_recursion();
  test_top_level_guarded_clause_function_self_recursion();
  test_top_level_clause_function_self_recursion_base_one_orders();
  test_top_level_clause_function_miss_raises_match_error();
  test_branching_and_last_result();
  test_manual_closure_call_and_capture();
  test_runtime_uninitialized_register_read_raises_name_error();
  test_manual_call_invokes_object_call_method();
  test_execute_emitted_send_method();
  test_execute_emitted_integer_specialized_ops();
  test_execute_emitted_numeric_equality_and_new_ops();
  test_execute_emitted_integer_send_fast_path_new_ops();
  test_runtime_integer_sidecar_result_returns_materialized_value();
  test_runtime_integer_sidecar_materializes_completed_local();
  test_execute_emitted_compare_method();
  test_execute_emitted_default_method();
  test_execute_emitted_keyword_method();
  test_execute_emitted_block_send();
  test_execute_emitted_properties();
  test_bare_nullary_member_implicit_call();
  test_bare_non_nullary_member_rejected();
  test_property_called_as_method_rejected();
  test_dot_call_invokes_member_result();
  test_property_access_errors();
  test_property_arm_is_non_suspendable();
  test_kwargs_spread_requires_property();
  test_runtime_duplicate_keyword_values_are_read_before_duplicate_check();
  test_runtime_keyword_shape_cache_canonicalizes_keyword_order();
  test_runtime_call_cache_distinguishes_block_presence();
  test_runtime_keyword_call_cache_invalidates_on_world_epoch();
  test_manual_dynamic_send();
  test_manual_integer_opcode_float_fallback();
  test_execute_emitted_class_method_send();
  test_execute_emitted_constructor_call();
  test_execute_emitted_constructor_auto_assign();
  test_execute_emitted_constructor_default();
  test_execute_emitted_cvar_store_and_load();
  test_execute_emitted_constructor_cvar_auto_assign();
  test_execute_emitted_superclass_dispatch();
  test_execute_emitted_include_linearization();
  test_execute_emitted_extend_linearization();
  test_execute_emitted_method_missing_instance();
  test_execute_emitted_method_missing_class_side();
  test_method_missing_does_not_recurse();
  test_execute_emitted_case_literal();
  test_execute_emitted_case_pin();
  test_execute_emitted_case_bind();
  test_execute_emitted_case_bang_failure();
  test_execute_emitted_case_const_class();
  test_execute_emitted_case_list_exact();
  test_execute_emitted_pattern_assign_list_rest();
  test_execute_emitted_case_map_rest();
  test_execute_emitted_case_map_strict_null();
  test_execute_emitted_clause_method_dispatch();
  test_runtime_capability_checks();
  test_runtime_effect_checks();
  test_runtime_replay_trace_recording_and_divergence();
  test_runtime_schema_and_table_metadata();
  test_runtime_wasm_and_accelerator_metadata();
  test_runtime_modern_profile_metadata();
  test_manual_make_map();
  test_execute_emitted_block_map_suffixes();
  test_execute_emitted_v20_5_array_generation_and_optional_access();
  test_runtime_watch_local_storage_replacement();
  test_runtime_integer_specialized_op_preserves_watch_local_write();
  test_runtime_compare_branch_preserves_debug_local();
  test_runtime_watch_capture_uses_shared_storage_cell();
  test_runtime_watch_ivar_records_object_revision_events();
  test_runtime_dependency_capture_records_binding_reads();
  test_runtime_dependency_capture_deduplicates_repeated_binding_reads();
  test_runtime_dependency_capture_records_nested_ivar_reads();
  test_runtime_sequence_collections_contract();
  test_runtime_map_collections_contract();
  test_manual_instance_send_dispatch();
  test_manual_store_and_load_ivar();
  test_manual_store_and_load_cvar();
  test_manual_multi_segment_lookup_const();
  test_manual_multi_segment_superclass_dispatch();
  test_manual_send_cache_receiver_class_guard();
  test_manual_ivar_cache_shape_guard();
  test_runtime_ivar_shape_slot_transition_stability();
  test_runtime_dead_shape_rejects_ivar_access();
  test_runtime_heap_worker_arena_headers();
  test_runtime_heap_remote_free_queue_drains_on_owner();
  test_runtime_heap_allocation_heavy_smoke();
  test_runtime_gc_full_cycle_preserves_root_address();
  test_runtime_gc_reclaims_unrooted_reference_cycle();
  test_runtime_gc_write_barrier_remembers_mature_to_young_edge();
  test_runtime_gc_write_barrier_rejects_invalid_edges();
  test_runtime_gc_safepoint_scans_vm_frame_roots();
  test_runtime_gc_safepoint_preserves_caller_roots_during_call();
  test_runtime_gc_backedge_safepoint_preserves_live_roots();
  test_runtime_gc_preserves_rooted_local_and_shared_cycles();
  test_runtime_gc_parallel_smoke();
  test_runtime_pin_roots_gc_and_rejects_stale_unpin();
  test_runtime_pin_scope_nesting_counts_and_releases();
  test_runtime_pin_scope_releases_during_exception_unwind();
  test_runtime_pin_opaque_handle_boundary();
  test_runtime_pin_buffer_view_mode();
  test_runtime_pin_dealloc_after_pin_violation();
  test_runtime_pin_parallel_race_smoke();
  test_runtime_native_wait_cancel_poll_uses_active_pin();
  test_runtime_awaitable_select_ready_timeout_and_failure();
  test_runtime_awaitable_native_wait_pin_bridge_and_scheduler();
  test_runtime_awaitable_cancellation_finishes_native_wait();
  test_runtime_scheduler_runs_strands_in_parallel();
  test_runtime_scheduler_timer_queue_wakes_sleeping_strand();
  test_runtime_scheduler_explicit_wake_coalesces_sleeping_strand();
  test_runtime_task_join_rethrows_failure();
  test_runtime_task_join_timeout_does_not_cancel_task();
  test_runtime_task_cancel_is_cooperative_safepoint();
  test_runtime_structured_task_scope_propagates_first_failure();
  test_runtime_channel_rendezvous_fifo_close();
  test_runtime_channel_buffered_close_and_shareability_gate();
  test_runtime_move_slot_channel_transfer_and_moved_guard();
  test_runtime_select_rotates_ready_arms_and_handles_else_timeout();
  test_runtime_select_send_move_arm_commits_only_when_ready();
  test_runtime_supervisor_one_for_one_keeps_sibling_running();
  test_runtime_supervisor_one_for_all_cancels_all_siblings();
  test_runtime_supervisor_rest_for_one_cancels_later_siblings_only();
  test_runtime_mutex_non_reentrant_and_contention();
  test_runtime_atomic_seq_cst_compare_and_set_visibility();
  test_runtime_world_heap_tracks_vm_allocations();
  test_runtime_lifecycle_destroy_opcode_is_idempotent();
  test_runtime_lifecycle_dealloc_opcode_tombstones_instance_payload();
  test_runtime_lifecycle_dealloc_clears_collection_payload();
  test_runtime_world_define_method_invalidates_send_cache();
  test_runtime_world_include_invalidates_send_cache();
  test_runtime_world_transaction_replaces_mixin_method_for_cached_class();
  test_runtime_world_transaction_rolls_back_on_invalid_include();
  test_runtime_world_freeze_rejects_world_mutation_but_keeps_send();
  test_runtime_world_transaction_rejects_superclass_mismatch();
  test_runtime_world_transaction_rejects_include_cycle_before_commit();
  test_runtime_world_extend_invalidates_class_side_send_cache();
  test_runtime_reflection_mirrors_are_read_only_stable_and_ordered();
  test_runtime_package_reload_swaps_compatible_package_atomically();
  test_runtime_package_reload_rejects_incompatible_surface_without_swap();
  test_runtime_package_reload_rejects_frozen_world_without_swap();
  test_runtime_package_reload_rolls_back_failed_decode();
  test_manual_pattern_deconstruct_protocol_sequence();
  test_manual_pattern_deconstruct_protocol_map();
  test_source_try_rescue_ensure_execution();
  test_native_error_inherited_rescue_execution();
  test_source_throw_catch_execution();
  test_manual_raise_handler_table_recovers();
  test_manual_raise_unwinds_closure_to_outer_handler();
  test_manual_raise_unwinds_method_send_to_outer_handler();
  test_manual_ensure_suppresses_pending_exception();
  test_manual_raise_unhandled_fault_trace();
  test_foreign_handle_lifetime();
  test_native_tag_registry();
  std::cout << "vm_tests: ok\n";
  return 0;
}
