#include "bytecode/format.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib collections test failed: " << message << "\n";
    std::exit(1);
  }
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

std::uint32_t string_id_or_die(const amber::bytecode::BcModule &module,
                               const std::string &value) {
  for (std::uint32_t i = 0; i < module.strings.size(); ++i) {
    if (module.strings[i] == value) {
      return i;
    }
  }
  std::cerr << "stdlib collections test failed: missing string " << value
            << "\n";
  std::exit(1);
}

std::uint32_t symbol_id_or_die(const amber::bytecode::BcModule &module,
                               const std::string &name) {
  for (std::uint32_t i = 0; i < module.symbols.size(); ++i) {
    if (module.symbols[i] == name) {
      return i;
    }
  }
  std::cerr << "stdlib collections test failed: missing symbol " << name
            << "\n";
  std::exit(1);
}

std::uint32_t class_index_or_die(const amber::bytecode::BcModule &module,
                                 const std::string &name) {
  const std::uint32_t symbol_id = symbol_id_or_die(module, name);
  for (std::uint32_t i = 0; i < module.classes.size(); ++i) {
    if (module.classes[i].class_name_sym_id == symbol_id) {
      return i;
    }
  }
  std::cerr << "stdlib collections test failed: missing class " << name
            << "\n";
  std::exit(1);
}

std::uint32_t append_integer_const(amber::bytecode::BcModule *module,
                                   std::int64_t value) {
  amber::bytecode::Constant constant;
  constant.kind = amber::bytecode::ConstantKind::Integer;
  constant.int_value = value;
  module->const_pool.push_back(constant);
  return static_cast<std::uint32_t>(module->const_pool.size() - 1U);
}

std::uint32_t append_symbol_const(amber::bytecode::BcModule *module,
                                  const std::string &name) {
  amber::bytecode::Constant constant;
  constant.kind = amber::bytecode::ConstantKind::SymbolRef;
  constant.ref_id = ensure_symbol_id(module, name);
  module->const_pool.push_back(constant);
  return static_cast<std::uint32_t>(module->const_pool.size() - 1U);
}

amber::bytecode::Instruction
send_instr(std::uint32_t dst, std::uint32_t recv, std::uint32_t selector,
           const std::vector<std::uint32_t> &arg_regs = {},
           std::int64_t block_reg = -1,
           const std::vector<std::pair<std::uint32_t, std::uint32_t>>
               &kw_regs = {}) {
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
  insn.operands.push_back({0, false});
  return insn;
}

amber::bytecode::BcCode
make_send_code(std::uint32_t code_id, std::uint32_t selector, bool with_block) {
  amber::bytecode::BcCode code;
  code.code_id = code_id;
  code.kind = amber::bytecode::CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back(
      send_instr(2, 0, selector, {}, with_block ? 1 : -1));
  code.instructions.push_back({amber::bytecode::Opcode::Return, {{2, false}}});
  return code;
}

amber::bytecode::BcCode make_unary_send_code(std::uint32_t code_id,
                                             std::uint32_t selector) {
  amber::bytecode::BcCode code;
  code.code_id = code_id;
  code.kind = amber::bytecode::CodeKind::Method;
  code.reg_count = 3;
  code.instructions.push_back(send_instr(2, 0, selector, {1}));
  code.instructions.push_back({amber::bytecode::Opcode::Return, {{2, false}}});
  return code;
}

amber::runtime::Value
make_closure_value(std::uint32_t code_id,
                   std::vector<amber::runtime::Value> captures = {}) {
  auto closure = amber::runtime::make_intrusive<amber::runtime::ClosureValue>();
  closure->header.kind = amber::runtime::HeapObjectKind::Closure;
  closure->code_id = code_id;
  closure->captures = std::move(captures);
  return amber::runtime::Value::closure(std::move(closure));
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

amber::runtime::Value make_range_value(
    const amber::bytecode::BcModule &module, std::int64_t start,
    std::int64_t finish, bool inclusive_end = true, std::int64_t step = 1) {
  auto instance = amber::runtime::default_runtime_heap().make_instance_value(
      class_index_or_die(module, "Range"));
  instance->ivars["start"] = amber::runtime::Value::integer(start);
  instance->ivars["finish"] = amber::runtime::Value::integer(finish);
  instance->ivars["inclusive_end"] =
      amber::runtime::Value::boolean(inclusive_end);
  instance->ivars["step"] = amber::runtime::Value::integer(step);
  return amber::runtime::Value::instance(std::move(instance));
}

void expect_ok(const amber::runtime::ExecutionResult &result,
               const std::string &message) {
  expect(result.ok(), message + " should succeed");
}

void expect_fault(const amber::runtime::ExecutionResult &result,
                  const std::string &error_name, const std::string &message) {
  expect(!result.ok(), message + " should fault");
  expect(result.fault.has_value() && result.fault->error_name == error_name,
         message + " should report " + error_name);
}

void expect_integer(const amber::runtime::Value &value, std::int64_t expected,
                    const std::string &message) {
  expect(value.is_integer(), message + " should be integer");
  expect(value.as_integer() == expected, message + " value");
}

void expect_bool(const amber::runtime::Value &value, bool expected,
                 const std::string &message) {
  expect(value.is_bool(), message + " should be bool");
  expect(value.as_bool() == expected, message + " value");
}

void expect_integer_list(const amber::runtime::Value &value,
                         const std::vector<std::int64_t> &expected,
                         const std::string &message) {
  expect(value.is_list(), message + " should be a list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list = value.as_list();
  expect(list != nullptr, message + " list payload");
  expect(list->items.size() == expected.size(), message + " list size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect_integer(list->items[i], expected[i],
                   message + " item " + std::to_string(i));
  }
}

void expect_set_integer_items(const amber::runtime::Value &value,
                              const std::vector<std::int64_t> &expected,
                              const std::string &message) {
  expect(value.is_set(), message + " should be a set");
  const amber::runtime::IntrusivePtr<amber::runtime::SetValue> set = value.as_set();
  expect(set != nullptr, message + " set payload");
  expect(set->items.size() == expected.size(), message + " set size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect_integer(set->items[i], expected[i],
                   message + " item " + std::to_string(i));
  }
}

void expect_nested_integer_lists(
    const amber::runtime::Value &value,
    const std::vector<std::vector<std::int64_t>> &expected,
    const std::string &message) {
  expect(value.is_list(), message + " should be a list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> outer = value.as_list();
  expect(outer != nullptr, message + " outer payload");
  expect(outer->items.size() == expected.size(), message + " outer size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect_integer_list(outer->items[i], expected[i],
                        message + " row " + std::to_string(i));
  }
}

void expect_symbol_list(const amber::bytecode::BcModule &module,
                        const amber::runtime::Value &value,
                        const std::vector<std::string> &expected,
                        const std::string &message) {
  expect(value.is_list(), message + " should be a list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list = value.as_list();
  expect(list != nullptr, message + " list payload");
  expect(list->items.size() == expected.size(), message + " list size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect(list->items[i].is_symbol(), message + " item should be symbol");
    expect(list->items[i].as_symbol().symbol_id ==
               symbol_id_or_die(module, expected[i]),
           message + " item " + std::to_string(i));
  }
}

void expect_entry_list(
    const amber::bytecode::BcModule &module, const amber::runtime::Value &value,
    const std::vector<std::pair<std::string, std::int64_t>> &expected,
    const std::string &message) {
  expect(value.is_list(), message + " should be a list");
  const amber::runtime::IntrusivePtr<amber::runtime::ListValue> list = value.as_list();
  expect(list != nullptr, message + " list payload");
  expect(list->items.size() == expected.size(), message + " list size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect(list->items[i].is_tuple(),
           message + " entry " + std::to_string(i) + " should be tuple");
    const amber::runtime::IntrusivePtr<amber::runtime::TupleValue> tuple =
        list->items[i].as_tuple();
    expect(tuple != nullptr && tuple->items.size() == 2,
           message + " entry " + std::to_string(i) + " shape");
    expect(tuple->items[0].is_symbol(),
           message + " entry " + std::to_string(i) + " key should be symbol");
    expect(tuple->items[0].as_symbol().symbol_id ==
               symbol_id_or_die(module, expected[i].first),
           message + " entry " + std::to_string(i) + " key");
    expect_integer(tuple->items[1], expected[i].second,
                   message + " entry " + std::to_string(i) + " value");
  }
}

void expect_symbol_map_entries(
    const amber::bytecode::BcModule &module, const amber::runtime::Value &value,
    const std::vector<std::pair<std::string, std::int64_t>> &expected,
    const std::string &message) {
  expect(value.is_map(), message + " should be a map");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> map = value.as_map();
  expect(map != nullptr, message + " map payload");
  expect(map->entries.size() == expected.size(), message + " map size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect(map->entries[i].symbol_id ==
               symbol_id_or_die(module, expected[i].first),
           message + " entry " + std::to_string(i) + " key");
    expect_integer(map->entries[i].value, expected[i].second,
                   message + " entry " + std::to_string(i) + " value");
  }
}

amber::bytecode::BcModule make_sequence_protocol_module() {
  using namespace amber::bytecode;

  BcModule module;
  for (const std::string &symbol :
       {"each", "map", "flat_map", "select", "reject", "reduce", "find", "any?",
        "all?", "none?", "first", "count", "to_a", "to_array", "lazy",
        "group_by", "+", ">",
        "contains?", "include?", "===", "empty?", "[]", "Range", "low",
        "high", "collect", "collect_concat", "filter", "find_all", "detect",
        "inject", "member?", "length", "size", "entries", "times"}) {
    ensure_symbol_id(&module, symbol);
  }
  BcClass range_class;
  range_class.class_name_sym_id = symbol_id_or_die(module, "Range");
  module.classes.push_back(range_class);

  const std::uint32_t zero = append_integer_const(&module, 0);
  const std::uint32_t one = append_integer_const(&module, 1);
  const std::uint32_t three = append_integer_const(&module, 3);
  const std::uint32_t low = append_symbol_const(&module, "low");
  const std::uint32_t high = append_symbol_const(&module, "high");
  const std::uint32_t boom = append_symbol_const(&module, "Boom");

  module.code_objects.push_back(
      make_send_code(1, symbol_id_or_die(module, "each"), true));
  module.code_objects.push_back(
      make_send_code(2, symbol_id_or_die(module, "map"), true));
  module.code_objects.push_back(
      make_send_code(3, symbol_id_or_die(module, "flat_map"), true));
  module.code_objects.push_back(
      make_send_code(4, symbol_id_or_die(module, "select"), true));
  module.code_objects.push_back(
      make_send_code(5, symbol_id_or_die(module, "reject"), true));
  module.code_objects.push_back(
      make_send_code(7, symbol_id_or_die(module, "reduce"), true));
  module.code_objects.push_back(
      make_send_code(8, symbol_id_or_die(module, "find"), true));
  module.code_objects.push_back(
      make_send_code(9, symbol_id_or_die(module, "any?"), true));
  module.code_objects.push_back(
      make_send_code(10, symbol_id_or_die(module, "all?"), true));
  module.code_objects.push_back(
      make_send_code(11, symbol_id_or_die(module, "none?"), true));
  module.code_objects.push_back(
      make_send_code(12, symbol_id_or_die(module, "first"), false));
  module.code_objects.push_back(
      make_send_code(13, symbol_id_or_die(module, "count"), false));
  module.code_objects.push_back(
      make_send_code(14, symbol_id_or_die(module, "to_a"), false));
  module.code_objects.push_back(
      make_send_code(16, symbol_id_or_die(module, "group_by"), true));
  module.code_objects.push_back(
      make_send_code(17, symbol_id_or_die(module, "any?"), false));
  module.code_objects.push_back(
      make_send_code(18, symbol_id_or_die(module, "all?"), false));
  module.code_objects.push_back(
      make_send_code(19, symbol_id_or_die(module, "none?"), false));
  module.code_objects.push_back(
      make_unary_send_code(20, symbol_id_or_die(module, "contains?")));
  module.code_objects.push_back(
      make_unary_send_code(21, symbol_id_or_die(module, "===")));
  module.code_objects.push_back(
      make_send_code(22, symbol_id_or_die(module, "empty?"), false));
  module.code_objects.push_back(
      make_unary_send_code(23, symbol_id_or_die(module, "first")));
  module.code_objects.push_back(
      make_unary_send_code(24, symbol_id_or_die(module, "[]")));
  module.code_objects.push_back(
      make_unary_send_code(30, symbol_id_or_die(module, "include?")));
  module.code_objects.push_back(
      make_send_code(31, symbol_id_or_die(module, "collect"), true));
  module.code_objects.push_back(make_send_code(
      32, symbol_id_or_die(module, "collect_concat"), true));
  module.code_objects.push_back(
      make_send_code(33, symbol_id_or_die(module, "filter"), true));
  module.code_objects.push_back(
      make_send_code(34, symbol_id_or_die(module, "find_all"), true));
  module.code_objects.push_back(
      make_send_code(35, symbol_id_or_die(module, "detect"), true));
  module.code_objects.push_back(
      make_send_code(36, symbol_id_or_die(module, "inject"), true));
  module.code_objects.push_back(
      make_unary_send_code(37, symbol_id_or_die(module, "member?")));
  module.code_objects.push_back(
      make_send_code(38, symbol_id_or_die(module, "length"), false));
  module.code_objects.push_back(
      make_send_code(39, symbol_id_or_die(module, "size"), false));
  module.code_objects.push_back(
      make_send_code(40, symbol_id_or_die(module, "entries"), false));
  module.code_objects.push_back(
      make_send_code(41, symbol_id_or_die(module, "to_array"), false));
  module.code_objects.push_back(
      make_send_code(42, symbol_id_or_die(module, "times"), false));
  module.code_objects.push_back(
      make_send_code(43, symbol_id_or_die(module, "times"), true));

  BcCode reduce_init;
  reduce_init.code_id = 6;
  reduce_init.kind = CodeKind::Method;
  reduce_init.reg_count = 4;
  reduce_init.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {zero, false}}});
  reduce_init.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "reduce"), {2}, 1));
  reduce_init.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(reduce_init);

  BcCode lazy_map_to_a;
  lazy_map_to_a.code_id = 15;
  lazy_map_to_a.kind = CodeKind::Method;
  lazy_map_to_a.reg_count = 5;
  lazy_map_to_a.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "lazy")));
  lazy_map_to_a.instructions.push_back(
      send_instr(3, 2, symbol_id_or_die(module, "map"), {}, 1));
  lazy_map_to_a.instructions.push_back(
      send_instr(4, 3, symbol_id_or_die(module, "to_a")));
  lazy_map_to_a.instructions.push_back({Opcode::Return, {{4, false}}});
  module.code_objects.push_back(lazy_map_to_a);

  BcCode lazy_map_only;
  lazy_map_only.code_id = 25;
  lazy_map_only.kind = CodeKind::Method;
  lazy_map_only.reg_count = 4;
  lazy_map_only.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "lazy")));
  lazy_map_only.instructions.push_back(
      send_instr(3, 2, symbol_id_or_die(module, "map"), {}, 1));
  lazy_map_only.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(lazy_map_only);

  BcCode lazy_map_first;
  lazy_map_first.code_id = 26;
  lazy_map_first.kind = CodeKind::Method;
  lazy_map_first.reg_count = 5;
  lazy_map_first.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "lazy")));
  lazy_map_first.instructions.push_back(
      send_instr(3, 2, symbol_id_or_die(module, "map"), {}, 1));
  lazy_map_first.instructions.push_back(
      send_instr(4, 3, symbol_id_or_die(module, "first")));
  lazy_map_first.instructions.push_back({Opcode::Return, {{4, false}}});
  module.code_objects.push_back(lazy_map_first);

  BcCode lazy_map_first_count;
  lazy_map_first_count.code_id = 27;
  lazy_map_first_count.kind = CodeKind::Method;
  lazy_map_first_count.reg_count = 6;
  lazy_map_first_count.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {three, false}}});
  lazy_map_first_count.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "lazy")));
  lazy_map_first_count.instructions.push_back(
      send_instr(4, 3, symbol_id_or_die(module, "map"), {}, 1));
  lazy_map_first_count.instructions.push_back(
      send_instr(5, 4, symbol_id_or_die(module, "first"), {2}));
  lazy_map_first_count.instructions.push_back(
      {Opcode::Return, {{5, false}}});
  module.code_objects.push_back(lazy_map_first_count);

  BcCode lazy_map_select_to_a;
  lazy_map_select_to_a.code_id = 28;
  lazy_map_select_to_a.kind = CodeKind::Method;
  lazy_map_select_to_a.reg_count = 6;
  lazy_map_select_to_a.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "lazy")));
  lazy_map_select_to_a.instructions.push_back(
      send_instr(4, 3, symbol_id_or_die(module, "map"), {}, 1));
  lazy_map_select_to_a.instructions.push_back(
      send_instr(5, 4, symbol_id_or_die(module, "select"), {}, 2));
  lazy_map_select_to_a.instructions.push_back(
      send_instr(5, 5, symbol_id_or_die(module, "to_a")));
  lazy_map_select_to_a.instructions.push_back(
      {Opcode::Return, {{5, false}}});
  module.code_objects.push_back(lazy_map_select_to_a);

  BcCode lazy_flat_map_to_a;
  lazy_flat_map_to_a.code_id = 29;
  lazy_flat_map_to_a.kind = CodeKind::Method;
  lazy_flat_map_to_a.reg_count = 5;
  lazy_flat_map_to_a.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "lazy")));
  lazy_flat_map_to_a.instructions.push_back(
      send_instr(3, 2, symbol_id_or_die(module, "flat_map"), {}, 1));
  lazy_flat_map_to_a.instructions.push_back(
      send_instr(4, 3, symbol_id_or_die(module, "to_a")));
  lazy_flat_map_to_a.instructions.push_back({Opcode::Return, {{4, false}}});
  module.code_objects.push_back(lazy_flat_map_to_a);

  BcCode inc;
  inc.code_id = 100;
  inc.kind = CodeKind::Block;
  inc.reg_count = 3;
  inc.instructions.push_back({Opcode::LoadK, {{1, false}, {one, false}}});
  inc.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "+"), {1}));
  inc.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(inc);

  auto make_gt_block = [&](std::uint32_t code_id,
                           std::uint32_t constant_id) -> BcCode {
    BcCode block;
    block.code_id = code_id;
    block.kind = CodeKind::Block;
    block.reg_count = 3;
    block.instructions.push_back(
        {Opcode::LoadK, {{1, false}, {constant_id, false}}});
    block.instructions.push_back(
        send_instr(2, 0, symbol_id_or_die(module, ">"), {1}));
    block.instructions.push_back({Opcode::Return, {{2, false}}});
    return block;
  };
  module.code_objects.push_back(make_gt_block(101, one));
  module.code_objects.push_back(make_gt_block(104, zero));
  module.code_objects.push_back(make_gt_block(105, three));

  BcCode add;
  add.code_id = 102;
  add.kind = CodeKind::Block;
  add.reg_count = 3;
  add.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "+"), {1}));
  add.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(add);

  BcCode pairify;
  pairify.code_id = 103;
  pairify.kind = CodeKind::Block;
  pairify.reg_count = 3;
  pairify.instructions.push_back({Opcode::LoadK, {{1, false}, {one, false}}});
  pairify.instructions.push_back(
      send_instr(1, 0, symbol_id_or_die(module, "+"), {1}));
  pairify.instructions.push_back(
      {Opcode::MakeList, {{2, false}, {0, false}, {2, false}}});
  pairify.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(pairify);

  BcCode low_high_key;
  low_high_key.code_id = 106;
  low_high_key.kind = CodeKind::Block;
  low_high_key.reg_count = 5;
  low_high_key.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {one, false}}});
  low_high_key.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, ">"), {1}));
  low_high_key.instructions.push_back(
      {Opcode::JumpIfFalse, {{2, false}, {5, false}}});
  low_high_key.instructions.push_back(
      {Opcode::LoadK, {{3, false}, {high, false}}});
  low_high_key.instructions.push_back({Opcode::Return, {{3, false}}});
  low_high_key.instructions.push_back(
      {Opcode::LoadK, {{4, false}, {low, false}}});
  low_high_key.instructions.push_back({Opcode::Return, {{4, false}}});
  module.code_objects.push_back(low_high_key);

  BcCode raise_after_one;
  raise_after_one.code_id = 107;
  raise_after_one.kind = CodeKind::Block;
  raise_after_one.reg_count = 5;
  raise_after_one.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {one, false}}});
  raise_after_one.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, ">"), {1}));
  raise_after_one.instructions.push_back(
      {Opcode::JumpIfFalse, {{2, false}, {5, false}}});
  raise_after_one.instructions.push_back(
      {Opcode::LoadK, {{3, false}, {boom, false}}});
  raise_after_one.instructions.push_back({Opcode::Raise, {{3, false}}});
  raise_after_one.instructions.push_back(
      send_instr(4, 0, symbol_id_or_die(module, "+"), {1}));
  raise_after_one.instructions.push_back({Opcode::Return, {{4, false}}});
  module.code_objects.push_back(raise_after_one);

  return module;
}

void assert_sequence_protocol_for(const amber::bytecode::BcModule &module,
                                  const amber::runtime::Value &source,
                                  const std::string &label) {
  const amber::runtime::Value inc = make_closure_value(100);
  const amber::runtime::Value gt_one = make_closure_value(101);
  const amber::runtime::Value add = make_closure_value(102);
  const amber::runtime::Value pairify = make_closure_value(103);
  const amber::runtime::Value gt_zero = make_closure_value(104);
  const amber::runtime::Value gt_three = make_closure_value(105);
  const amber::runtime::Value low_high_key = make_closure_value(106);

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 1, {source, inc});
  expect_ok(result, label + " each");
  if (source.is_list()) {
    expect(result.value.is_list() && result.value.as_list() == source.as_list(),
           label + " each should return receiver");
  } else if (source.is_set()) {
    expect(result.value.is_set() && result.value.as_set() == source.as_set(),
           label + " each should return receiver");
  } else if (source.is_tuple()) {
    expect(result.value.is_tuple() &&
               result.value.as_tuple() == source.as_tuple(),
           label + " each should return receiver");
  } else {
    expect(result.value.is_instance_object() &&
               result.value.as_instance_object() ==
                   source.as_instance_object(),
           label + " each should return receiver");
  }

  result = amber::runtime::execute_code(module, 2, {source, inc});
  expect_ok(result, label + " map");
  expect_integer_list(result.value, {2, 3, 4}, label + " map");

  result = amber::runtime::execute_code(module, 3, {source, pairify});
  expect_ok(result, label + " flat_map");
  expect_integer_list(result.value, {1, 2, 2, 3, 3, 4}, label + " flat_map");

  result = amber::runtime::execute_code(module, 4, {source, gt_one});
  expect_ok(result, label + " select");
  expect_integer_list(result.value, {2, 3}, label + " select");

  result = amber::runtime::execute_code(module, 5, {source, gt_one});
  expect_ok(result, label + " reject");
  expect_integer_list(result.value, {1}, label + " reject");

  result = amber::runtime::execute_code(module, 6, {source, add});
  expect_ok(result, label + " reduce(init)");
  expect_integer(result.value, 6, label + " reduce(init)");

  result = amber::runtime::execute_code(module, 7, {source, add});
  expect_ok(result, label + " reduce");
  expect_integer(result.value, 6, label + " reduce");

  result = amber::runtime::execute_code(module, 8, {source, gt_one});
  expect_ok(result, label + " find");
  expect_integer(result.value, 2, label + " find");

  result = amber::runtime::execute_code(module, 9, {source, gt_one});
  expect_ok(result, label + " any?");
  expect_bool(result.value, true, label + " any?");

  result = amber::runtime::execute_code(module, 10, {source, gt_zero});
  expect_ok(result, label + " all?");
  expect_bool(result.value, true, label + " all?");

  result = amber::runtime::execute_code(module, 11, {source, gt_three});
  expect_ok(result, label + " none?");
  expect_bool(result.value, true, label + " none?");

  result = amber::runtime::execute_code(module, 12, {source});
  expect_ok(result, label + " first");
  expect_integer(result.value, 1, label + " first");

  result = amber::runtime::execute_code(module, 13, {source});
  expect_ok(result, label + " count");
  expect_integer(result.value, 3, label + " count");

  result = amber::runtime::execute_code(module, 14, {source});
  expect_ok(result, label + " to_a");
  expect_integer_list(result.value, {1, 2, 3}, label + " to_a");

  result = amber::runtime::execute_code(module, 15, {source, inc});
  expect_ok(result, label + " lazy materialization");
  expect_integer_list(result.value, {2, 3, 4}, label + " lazy materialization");

  result = amber::runtime::execute_code(module, 16, {source, low_high_key});
  expect_ok(result, label + " group_by");
  expect(result.value.is_map(), label + " group_by should return map");
  const amber::runtime::IntrusivePtr<amber::runtime::MapValue> groups =
      result.value.as_map();
  expect(groups != nullptr && groups->entries.size() == 2,
         label + " group_by shape");
  expect(groups->entries[0].symbol_id == symbol_id_or_die(module, "low"),
         label + " group_by low key");
  expect_integer_list(groups->entries[0].value, {1}, label + " group_by low");
  expect(groups->entries[1].symbol_id == symbol_id_or_die(module, "high"),
         label + " group_by high key");
  expect_integer_list(groups->entries[1].value, {2, 3},
                      label + " group_by high");

  result = amber::runtime::execute_code(module, 31, {source, inc});
  expect_ok(result, label + " collect alias");
  expect_integer_list(result.value, {2, 3, 4}, label + " collect alias");

  result = amber::runtime::execute_code(module, 32, {source, pairify});
  expect_ok(result, label + " collect_concat alias");
  expect_integer_list(result.value, {1, 2, 2, 3, 3, 4},
                      label + " collect_concat alias");

  result = amber::runtime::execute_code(module, 33, {source, gt_one});
  expect_ok(result, label + " filter alias");
  expect_integer_list(result.value, {2, 3}, label + " filter alias");

  result = amber::runtime::execute_code(module, 34, {source, gt_one});
  expect_ok(result, label + " find_all alias");
  expect_integer_list(result.value, {2, 3}, label + " find_all alias");

  result = amber::runtime::execute_code(module, 35, {source, gt_one});
  expect_ok(result, label + " detect alias");
  expect_integer(result.value, 2, label + " detect alias");

  result = amber::runtime::execute_code(module, 36, {source, add});
  expect_ok(result, label + " inject alias");
  expect_integer(result.value, 6, label + " inject alias");

  result = amber::runtime::execute_code(
      module, 37, {source, amber::runtime::Value::integer(2)});
  expect_ok(result, label + " member? alias");
  expect_bool(result.value, true, label + " member? alias");

  result = amber::runtime::execute_code(module, 38, {source});
  expect_ok(result, label + " length alias");
  expect_integer(result.value, 3, label + " length alias");

  result = amber::runtime::execute_code(module, 39, {source});
  expect_ok(result, label + " size alias");
  expect_integer(result.value, 3, label + " size alias");

  result = amber::runtime::execute_code(module, 40, {source});
  expect_ok(result, label + " entries alias");
  expect_integer_list(result.value, {1, 2, 3}, label + " entries alias");
}

void test_std001_sequence_protocol_matrix() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const std::vector<amber::runtime::Value> items = {
      amber::runtime::Value::integer(1), amber::runtime::Value::integer(2),
      amber::runtime::Value::integer(3)};

  assert_sequence_protocol_for(module, amber::runtime::make_list_value(items),
                               "Array");
  assert_sequence_protocol_for(module, amber::runtime::make_tuple_value(items),
                               "Tuple");
  assert_sequence_protocol_for(module, amber::runtime::make_set_value(items),
                               "Set");
}

void test_std002_range_eager_methods() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  assert_sequence_protocol_for(module, make_range_value(module, 1, 3),
                               "Range");
}

void test_std001_empty_sequence_edges() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const amber::runtime::Value add = make_closure_value(102);

  for (const auto &entry : {std::pair<std::string, amber::runtime::Value>{
                                "Array", amber::runtime::make_list_value({})},
                            {"Tuple", amber::runtime::make_tuple_value({})},
                            {"Set", amber::runtime::make_set_value({})}}) {
    const std::string &label = entry.first;
    const amber::runtime::Value &source = entry.second;

    amber::runtime::ExecutionResult result =
        amber::runtime::execute_code(module, 7, {source, add});
    expect_fault(result, "EmptyCollectionError",
                 label + " empty reduce without init");

    result = amber::runtime::execute_code(module, 12, {source});
    expect_ok(result, label + " empty first");
    expect(result.value.is_null(), label + " empty first should be null");

    result = amber::runtime::execute_code(module, 13, {source});
    expect_ok(result, label + " empty count");
    expect_integer(result.value, 0, label + " empty count");

    result = amber::runtime::execute_code(module, 17, {source});
    expect_ok(result, label + " empty any?");
    expect_bool(result.value, false, label + " empty any?");

    result = amber::runtime::execute_code(module, 18, {source});
    expect_ok(result, label + " empty all?");
    expect_bool(result.value, true, label + " empty all?");

    result = amber::runtime::execute_code(module, 19, {source});
    expect_ok(result, label + " empty none?");
    expect_bool(result.value, true, label + " empty none?");
  }
}

void test_std002_empty_range_edges() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const amber::runtime::Value add = make_closure_value(102);
  const amber::runtime::Value empty = make_range_value(module, 3, 1);

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 7, {empty, add});
  expect_fault(result, "EmptyCollectionError",
               "Range empty reduce without init");

  result = amber::runtime::execute_code(module, 12, {empty});
  expect_ok(result, "Range empty first");
  expect(result.value.is_null(), "Range empty first should be null");

  result = amber::runtime::execute_code(module, 13, {empty});
  expect_ok(result, "Range empty count");
  expect_integer(result.value, 0, "Range empty count");

  result = amber::runtime::execute_code(module, 17, {empty});
  expect_ok(result, "Range empty any?");
  expect_bool(result.value, false, "Range empty any?");

  result = amber::runtime::execute_code(module, 18, {empty});
  expect_ok(result, "Range empty all?");
  expect_bool(result.value, true, "Range empty all?");

  result = amber::runtime::execute_code(module, 19, {empty});
  expect_ok(result, "Range empty none?");
  expect_bool(result.value, true, "Range empty none?");
}

void test_std002_range_exclusive_and_open_end_edges() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const amber::runtime::Value inclusive = make_range_value(module, 1, 3);
  const amber::runtime::Value exclusive = make_range_value(module, 1, 3, false);
  const amber::runtime::Value open_end =
      make_range_value(module, 4, 0, true);
  open_end.as_instance_object()->ivars["finish"] =
      amber::runtime::Value::null();

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 20,
                                   {inclusive,
                                    amber::runtime::Value::integer(3)});
  expect_ok(result, "Range inclusive contains?");
  expect_bool(result.value, true, "Range inclusive contains?");

  result = amber::runtime::execute_code(
      module, 20, {exclusive, amber::runtime::Value::integer(3)});
  expect_ok(result, "Range exclusive contains?");
  expect_bool(result.value, false, "Range exclusive contains?");

  result = amber::runtime::execute_code(module, 21,
                                        {exclusive,
                                         amber::runtime::Value::integer(3)});
  expect_ok(result, "Range exclusive === finish");
  expect_bool(result.value, false, "Range exclusive === finish");

  result = amber::runtime::execute_code(module, 14, {exclusive});
  expect_ok(result, "Range exclusive to_a");
  expect_integer_list(result.value, {1, 2}, "Range exclusive to_a");

  result = amber::runtime::execute_code(module, 41, {exclusive});
  expect_ok(result, "Range exclusive to_array");
  expect_integer_list(result.value, {1, 2}, "Range exclusive to_array");

  const amber::runtime::Value stepped =
      make_range_value(module, 1, 5, true, 2);
  result = amber::runtime::execute_code(module, 41, {stepped});
  expect_ok(result, "Range stepped to_array");
  expect_integer_list(result.value, {1, 3, 5}, "Range stepped to_array");

  const amber::runtime::Value descending =
      make_range_value(module, 5, 1, true, -2);
  result = amber::runtime::execute_code(module, 41, {descending});
  expect_ok(result, "Range descending to_array");
  expect_integer_list(result.value, {5, 3, 1}, "Range descending to_array");

  result = amber::runtime::execute_code(
      module, 20, {open_end, amber::runtime::Value::integer(100)});
  expect_ok(result, "Range open-end contains?");
  expect_bool(result.value, true, "Range open-end contains?");

  result = amber::runtime::execute_code(
      module, 30, {open_end, amber::runtime::Value::integer(100)});
  expect_ok(result, "Range open-end include?");
  expect_bool(result.value, true, "Range open-end include?");

  result = amber::runtime::execute_code(module, 12, {open_end});
  expect_ok(result, "Range open-end first");
  expect_integer(result.value, 4, "Range open-end first");

  result = amber::runtime::execute_code(
      module, 23, {open_end, amber::runtime::Value::integer(3)});
  expect_ok(result, "Range open-end first(count)");
  expect_integer_list(result.value, {4, 5, 6},
                      "Range open-end first(count)");

  result = amber::runtime::execute_code(
      module, 24, {open_end, amber::runtime::Value::integer(2)});
  expect_ok(result, "Range open-end []");
  expect_integer(result.value, 6, "Range open-end []");

  result = amber::runtime::execute_code(module, 14, {open_end});
  expect_fault(result, "InfiniteCollectionError",
               "Range open-end eager to_a");

  const amber::runtime::Value open_begin = make_range_value(module, 0, 5);
  open_begin.as_instance_object()->ivars["start"] =
      amber::runtime::Value::null();
  result = amber::runtime::execute_code(
      module, 20, {open_begin, amber::runtime::Value::integer(-100)});
  expect_ok(result, "Range open-begin contains?");
  expect_bool(result.value, true, "Range open-begin contains?");
}

void test_std002_int_times() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const amber::runtime::Value inc = make_closure_value(100);

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 42,
                                   {amber::runtime::Value::integer(5)});
  expect_ok(result, "Int#times enumerable");
  expect_integer_list(result.value, {0, 1, 2, 3, 4},
                      "Int#times enumerable");

  result = amber::runtime::execute_code(module, 42,
                                        {amber::runtime::Value::integer(0)});
  expect_ok(result, "zero Int#times enumerable");
  expect_integer_list(result.value, {}, "zero Int#times enumerable");

  result = amber::runtime::execute_code(module, 42,
                                        {amber::runtime::Value::integer(-3)});
  expect_ok(result, "negative Int#times enumerable");
  expect_integer_list(result.value, {}, "negative Int#times enumerable");

  result = amber::runtime::execute_code(module, 43,
                                        {amber::runtime::Value::integer(3),
                                         inc});
  expect_ok(result, "direct block Int#times");
  expect(result.value.is_null(), "direct block Int#times returns null");

  const amber::runtime::Value times =
      amber::runtime::make_list_value({amber::runtime::Value::integer(0),
                                       amber::runtime::Value::integer(1),
                                       amber::runtime::Value::integer(2)});
  result = amber::runtime::execute_code(module, 2, {times, inc});
  expect_ok(result, "times map equivalent");
  expect_integer_list(result.value, {1, 2, 3}, "times map equivalent");
}

void test_std003_lazy_pipeline_and_materialization() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const amber::runtime::Value source = amber::runtime::make_list_value(
      {amber::runtime::Value::integer(1), amber::runtime::Value::integer(2),
       amber::runtime::Value::integer(3)});
  const amber::runtime::Value inc = make_closure_value(100);
  const amber::runtime::Value gt_one = make_closure_value(101);
  const amber::runtime::Value pairify = make_closure_value(103);
  const amber::runtime::Value raise_after_one = make_closure_value(107);

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 25, {source, raise_after_one});
  expect_ok(result, "LazySeq pipeline construction");
  expect(result.value.is_instance_object(),
         "LazySeq pipeline construction should return wrapper");

  result = amber::runtime::execute_code(module, 26, {source, raise_after_one});
  expect_ok(result, "LazySeq first short-circuit");
  expect_integer(result.value, 2, "LazySeq first short-circuit");

  result = amber::runtime::execute_code(module, 15, {source, raise_after_one});
  expect_fault(result, "Boom", "LazySeq to_a materialization");

  result = amber::runtime::execute_code(module, 28, {source, inc, gt_one});
  expect_ok(result, "LazySeq map/select/to_a pipeline");
  expect_integer_list(result.value, {2, 3, 4},
                      "LazySeq map/select/to_a pipeline");

  result = amber::runtime::execute_code(module, 29, {source, pairify});
  expect_ok(result, "LazySeq flat_map/to_a pipeline");
  expect_integer_list(result.value, {1, 2, 2, 3, 3, 4},
                      "LazySeq flat_map/to_a pipeline");

  amber::runtime::Value open_end = make_range_value(module, 4, 0, true);
  open_end.as_instance_object()->ivars["finish"] =
      amber::runtime::Value::null();
  result = amber::runtime::execute_code(module, 27, {open_end, inc});
  expect_ok(result, "LazySeq open-ended Range first(count)");
  expect_integer_list(result.value, {5, 6, 7},
                      "LazySeq open-ended Range first(count)");

  result = amber::runtime::execute_code(module, 15, {open_end, inc});
  expect_fault(result, "InfiniteCollectionError",
               "LazySeq open-ended to_a");
}

amber::bytecode::BcModule make_std005_collection_ops_module() {
  using namespace amber::bytecode;

  BcModule module;
  for (const std::string &symbol :
       {"union", "intersection", "difference", "left_difference",
        "symmetric_difference", "subset?", "proper_subset?", "superset?",
        "proper_superset?", "disjoint?", "contains?", "include?",
        "permutation", "combination", "merge", "&", "|", "+", "-", "^", "*",
        "<=", "<", ">=", ">", "concat", "take_while", "reverse", "sort",
        "uniq", "each", "each_pair", "each_cons", "each_slice", "step",
        "alpha", "beta", "gamma"}) {
    ensure_symbol_id(&module, symbol);
  }
  const std::uint32_t two = append_integer_const(&module, 2);
  const std::uint32_t four = append_integer_const(&module, 4);

  module.code_objects.push_back(
      make_unary_send_code(1, symbol_id_or_die(module, "union")));
  module.code_objects.push_back(
      make_unary_send_code(2, symbol_id_or_die(module, "intersection")));
  module.code_objects.push_back(
      make_unary_send_code(3, symbol_id_or_die(module, "difference")));
  module.code_objects.push_back(
      make_unary_send_code(4, symbol_id_or_die(module, "left_difference")));
  module.code_objects.push_back(make_unary_send_code(
      5, symbol_id_or_die(module, "symmetric_difference")));
  module.code_objects.push_back(
      make_unary_send_code(6, symbol_id_or_die(module, "subset?")));
  module.code_objects.push_back(
      make_unary_send_code(7, symbol_id_or_die(module, "proper_subset?")));
  module.code_objects.push_back(
      make_unary_send_code(8, symbol_id_or_die(module, "superset?")));
  module.code_objects.push_back(
      make_unary_send_code(9, symbol_id_or_die(module, "proper_superset?")));
  module.code_objects.push_back(
      make_unary_send_code(10, symbol_id_or_die(module, "disjoint?")));
  module.code_objects.push_back(
      make_unary_send_code(11, symbol_id_or_die(module, "contains?")));
  module.code_objects.push_back(
      make_unary_send_code(12, symbol_id_or_die(module, "include?")));
  module.code_objects.push_back(
      make_unary_send_code(13, symbol_id_or_die(module, "permutation")));
  module.code_objects.push_back(
      make_unary_send_code(14, symbol_id_or_die(module, "combination")));
  module.code_objects.push_back(
      make_unary_send_code(15, symbol_id_or_die(module, "merge")));

  BcCode merge_with_block;
  merge_with_block.code_id = 16;
  merge_with_block.kind = CodeKind::Method;
  merge_with_block.reg_count = 4;
  merge_with_block.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "merge"), {1}, 2));
  merge_with_block.instructions.push_back(
      {Opcode::Return, {{3, false}}});
  module.code_objects.push_back(merge_with_block);

  module.code_objects.push_back(
      make_unary_send_code(17, symbol_id_or_die(module, "&")));
  module.code_objects.push_back(
      make_unary_send_code(18, symbol_id_or_die(module, "|")));
  module.code_objects.push_back(
      make_unary_send_code(19, symbol_id_or_die(module, "+")));
  module.code_objects.push_back(
      make_unary_send_code(20, symbol_id_or_die(module, "-")));
  module.code_objects.push_back(
      make_unary_send_code(21, symbol_id_or_die(module, "^")));
  module.code_objects.push_back(
      make_unary_send_code(22, symbol_id_or_die(module, "<=")));
  module.code_objects.push_back(
      make_unary_send_code(23, symbol_id_or_die(module, "<")));
  module.code_objects.push_back(
      make_unary_send_code(24, symbol_id_or_die(module, ">=")));
  module.code_objects.push_back(
      make_unary_send_code(25, symbol_id_or_die(module, ">")));
  module.code_objects.push_back(
      make_unary_send_code(26, symbol_id_or_die(module, "concat")));
  module.code_objects.push_back(
      make_unary_send_code(27, symbol_id_or_die(module, "*")));
  module.code_objects.push_back(
      make_send_code(28, symbol_id_or_die(module, "take_while"), true));
  module.code_objects.push_back(
      make_send_code(29, symbol_id_or_die(module, "reverse"), false));
  module.code_objects.push_back(
      make_send_code(30, symbol_id_or_die(module, "sort"), false));
  module.code_objects.push_back(
      make_send_code(31, symbol_id_or_die(module, "sort"), true));
  module.code_objects.push_back(
      make_send_code(32, symbol_id_or_die(module, "uniq"), false));
  module.code_objects.push_back(
      make_send_code(33, symbol_id_or_die(module, "uniq"), true));
  module.code_objects.push_back(
      make_send_code(34, symbol_id_or_die(module, "each_pair"), false));
  module.code_objects.push_back(
      make_unary_send_code(35, symbol_id_or_die(module, "each_cons")));

  BcCode each_window_step;
  each_window_step.code_id = 36;
  each_window_step.kind = CodeKind::Method;
  each_window_step.reg_count = 5;
  each_window_step.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {two, false}}});
  each_window_step.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "each"), {1}, -1,
                 {{symbol_id_or_die(module, "step"), 2}}));
  each_window_step.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(each_window_step);

  module.code_objects.push_back(
      make_unary_send_code(37, symbol_id_or_die(module, "|")));
  module.code_objects.push_back(
      make_unary_send_code(38, symbol_id_or_die(module, "+")));
  module.code_objects.push_back(
      make_unary_send_code(39, symbol_id_or_die(module, "each_slice")));

  BcCode merge_sum;
  merge_sum.code_id = 100;
  merge_sum.kind = CodeKind::Block;
  merge_sum.reg_count = 4;
  merge_sum.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, "+"), {2}));
  merge_sum.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(merge_sum);

  BcCode sort_desc;
  sort_desc.code_id = 101;
  sort_desc.kind = CodeKind::Block;
  sort_desc.reg_count = 3;
  sort_desc.instructions.push_back(
      send_instr(2, 1, symbol_id_or_die(module, "-"), {0}));
  sort_desc.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(sort_desc);

  BcCode greater_than_two;
  greater_than_two.code_id = 102;
  greater_than_two.kind = CodeKind::Block;
  greater_than_two.reg_count = 3;
  greater_than_two.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {two, false}}});
  greater_than_two.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, ">"), {1}));
  greater_than_two.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(greater_than_two);

  BcCode less_than_four;
  less_than_four.code_id = 103;
  less_than_four.kind = CodeKind::Block;
  less_than_four.reg_count = 3;
  less_than_four.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {four, false}}});
  less_than_four.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "<"), {1}));
  less_than_four.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(less_than_four);

  return module;
}

void test_std005_collection_operations() {
  const amber::bytecode::BcModule module = make_std005_collection_ops_module();
  auto integer = [](std::int64_t value) {
    return amber::runtime::Value::integer(value);
  };

  const amber::runtime::Value left =
      amber::runtime::make_list_value({integer(1), integer(2), integer(3),
                                       integer(2)});
  const amber::runtime::Value right =
      amber::runtime::make_list_value({integer(3), integer(4), integer(2)});

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 1, {left, right});
  expect_ok(result, "Array#union");
  expect_integer_list(result.value, {1, 2, 3, 4}, "Array#union");

  result = amber::runtime::execute_code(module, 2, {left, right});
  expect_ok(result, "Array#intersection");
  expect_integer_list(result.value, {2, 3}, "Array#intersection");

  result = amber::runtime::execute_code(module, 3, {left, right});
  expect_ok(result, "Array#difference");
  expect_integer_list(result.value, {1}, "Array#difference");

  result = amber::runtime::execute_code(module, 4, {left, right});
  expect_ok(result, "Array#left_difference");
  expect_integer_list(result.value, {1}, "Array#left_difference");

  result = amber::runtime::execute_code(module, 5, {left, right});
  expect_ok(result, "Array#symmetric_difference");
  expect_integer_list(result.value, {1, 4}, "Array#symmetric_difference");

  const amber::runtime::Value small =
      amber::runtime::make_list_value({integer(1), integer(2)});
  result = amber::runtime::execute_code(module, 6, {small, left});
  expect_ok(result, "Array#subset?");
  expect_bool(result.value, true, "Array#subset?");

  result = amber::runtime::execute_code(module, 7, {small, left});
  expect_ok(result, "Array#proper_subset?");
  expect_bool(result.value, true, "Array#proper_subset?");

  result = amber::runtime::execute_code(module, 8, {left, small});
  expect_ok(result, "Array#superset?");
  expect_bool(result.value, true, "Array#superset?");

  result = amber::runtime::execute_code(module, 9, {left, small});
  expect_ok(result, "Array#proper_superset?");
  expect_bool(result.value, true, "Array#proper_superset?");

  const amber::runtime::Value far =
      amber::runtime::make_list_value({integer(8), integer(9)});
  result = amber::runtime::execute_code(module, 10, {left, far});
  expect_ok(result, "Array#disjoint?");
  expect_bool(result.value, true, "Array#disjoint?");

  result = amber::runtime::execute_code(module, 11, {left, integer(3)});
  expect_ok(result, "Array#contains?");
  expect_bool(result.value, true, "Array#contains?");

  result = amber::runtime::execute_code(module, 12, {left, integer(99)});
  expect_ok(result, "Array#include?");
  expect_bool(result.value, false, "Array#include?");

  const amber::runtime::Value set_left =
      amber::runtime::make_set_value({integer(2), integer(3)});
  const amber::runtime::Value set_right =
      amber::runtime::make_set_value({integer(3), integer(4)});
  result = amber::runtime::execute_code(module, 1, {set_left, set_right});
  expect_ok(result, "Set#union");
  expect_set_integer_items(result.value, {2, 3, 4}, "Set#union");

  const amber::runtime::Value ordered =
      amber::runtime::make_list_value({integer(1), integer(2), integer(3)});
  result = amber::runtime::execute_code(module, 13, {ordered, integer(2)});
  expect_ok(result, "Array#permutation");
  expect_nested_integer_lists(result.value,
                              {{1, 2}, {1, 3}, {2, 1}, {2, 3}, {3, 1},
                               {3, 2}},
                              "Array#permutation");

  result = amber::runtime::execute_code(module, 14, {ordered, integer(2)});
  expect_ok(result, "Array#combination");
  expect_nested_integer_lists(result.value, {{1, 2}, {1, 3}, {2, 3}},
                              "Array#combination");

  const amber::runtime::Value left_map =
      make_symbol_map(module, {{"alpha", integer(1)}, {"beta", integer(2)}});
  const amber::runtime::Value right_map =
      make_symbol_map(module, {{"beta", integer(20)}, {"gamma", integer(3)}});

  result = amber::runtime::execute_code(module, 15, {left_map, right_map});
  expect_ok(result, "Map#merge");
  expect_symbol_map_entries(module, result.value,
                            {{"alpha", 1}, {"beta", 20}, {"gamma", 3}},
                            "Map#merge");

  result = amber::runtime::execute_code(
      module, 16, {left_map, right_map, make_closure_value(100)});
  expect_ok(result, "Map#merge block");
  expect_symbol_map_entries(module, result.value,
                            {{"alpha", 1}, {"beta", 22}, {"gamma", 3}},
                            "Map#merge block");

  result = amber::runtime::execute_code(module, 17, {left, right});
  expect_ok(result, "Array#&");
  expect_integer_list(result.value, {2, 3}, "Array#&");

  result = amber::runtime::execute_code(module, 18, {left, right});
  expect_ok(result, "Array#|");
  expect_integer_list(result.value, {1, 2, 3, 4}, "Array#|");

  result = amber::runtime::execute_code(module, 19, {left, right});
  expect_ok(result, "Array#+");
  expect_integer_list(result.value, {1, 2, 3, 2, 3, 4, 2}, "Array#+");

  result = amber::runtime::execute_code(module, 20, {left, right});
  expect_ok(result, "Array#-");
  expect_integer_list(result.value, {1}, "Array#-");

  result = amber::runtime::execute_code(module, 21, {left, right});
  expect_ok(result, "Array#^");
  expect_integer_list(result.value, {1, 4}, "Array#^");

  result = amber::runtime::execute_code(module, 22, {small, left});
  expect_ok(result, "Array#<=");
  expect_bool(result.value, true, "Array#<=");

  result = amber::runtime::execute_code(module, 23, {small, left});
  expect_ok(result, "Array#<");
  expect_bool(result.value, true, "Array#<");

  result = amber::runtime::execute_code(module, 24, {left, small});
  expect_ok(result, "Array#>=");
  expect_bool(result.value, true, "Array#>=");

  result = amber::runtime::execute_code(module, 25, {left, small});
  expect_ok(result, "Array#>");
  expect_bool(result.value, true, "Array#>");

  result = amber::runtime::execute_code(module, 26, {small, far});
  expect_ok(result, "Array#concat");
  expect_integer_list(result.value, {1, 2, 8, 9}, "Array#concat");

  result = amber::runtime::execute_code(module, 27, {small, integer(3)});
  expect_ok(result, "Array#*");
  expect_integer_list(result.value, {1, 2, 1, 2, 1, 2}, "Array#*");

  const amber::runtime::Value four_items = amber::runtime::make_list_value(
      {integer(1), integer(2), integer(3), integer(4)});
  result = amber::runtime::execute_code(module, 28,
                                        {four_items, make_closure_value(103)});
  expect_ok(result, "Array#take_while");
  expect_integer_list(result.value, {1, 2, 3}, "Array#take_while");

  result = amber::runtime::execute_code(module, 29, {ordered});
  expect_ok(result, "Array#reverse");
  expect_integer_list(result.value, {3, 2, 1}, "Array#reverse");

  const amber::runtime::Value unsorted =
      amber::runtime::make_list_value({integer(3), integer(1), integer(2)});
  result = amber::runtime::execute_code(module, 30, {unsorted});
  expect_ok(result, "Array#sort");
  expect_integer_list(result.value, {1, 2, 3}, "Array#sort");

  result = amber::runtime::execute_code(module, 31,
                                        {unsorted, make_closure_value(101)});
  expect_ok(result, "Array#sort block");
  expect_integer_list(result.value, {3, 2, 1}, "Array#sort block");

  const amber::runtime::Value duplicates = amber::runtime::make_list_value(
      {integer(1), integer(2), integer(1), integer(3), integer(2)});
  result = amber::runtime::execute_code(module, 32, {duplicates});
  expect_ok(result, "Array#uniq");
  expect_integer_list(result.value, {1, 2, 3}, "Array#uniq");

  result = amber::runtime::execute_code(module, 33,
                                        {four_items, make_closure_value(102)});
  expect_ok(result, "Array#uniq block");
  expect_integer_list(result.value, {1, 3}, "Array#uniq block");

  result = amber::runtime::execute_code(module, 34, {ordered});
  expect_ok(result, "Array#each_pair");
  expect_nested_integer_lists(result.value, {{1, 2}, {2, 3}},
                              "Array#each_pair");

  result = amber::runtime::execute_code(module, 35, {four_items, integer(3)});
  expect_ok(result, "Array#each_cons");
  expect_nested_integer_lists(result.value, {{1, 2, 3}, {2, 3, 4}},
                              "Array#each_cons");

  result = amber::runtime::execute_code(module, 36, {four_items, integer(2)});
  expect_ok(result, "Array#each step");
  expect_nested_integer_lists(result.value, {{1, 2}, {3, 4}},
                              "Array#each step");

  result = amber::runtime::execute_code(module, 39, {four_items, integer(2)});
  expect_ok(result, "Array#each_slice");
  expect_nested_integer_lists(result.value, {{1, 2}, {3, 4}},
                              "Array#each_slice");

  result = amber::runtime::execute_code(module, 37, {left_map, right_map});
  expect_ok(result, "Map#|");
  expect_symbol_map_entries(module, result.value,
                            {{"alpha", 1}, {"beta", 20}, {"gamma", 3}},
                            "Map#|");

  result = amber::runtime::execute_code(module, 38, {left_map, right_map});
  expect_ok(result, "Map#+");
  expect_symbol_map_entries(module, result.value,
                            {{"alpha", 1}, {"beta", 20}, {"gamma", 3}},
                            "Map#+");

  result = amber::runtime::execute_code(module, 34, {left_map});
  expect_ok(result, "Map#each_pair");
  expect_entry_list(module, result.value, {{"alpha", 1}, {"beta", 2}},
                    "Map#each_pair");
}

amber::bytecode::BcModule make_map_protocol_module() {
  using namespace amber::bytecode;

  BcModule module;
  for (const std::string &symbol :
       {"keys", "values", "entries", "to_a", "each", "map", "select", "reject",
        "transform", "transform_values", "[]", "contains?", "include?", "+",
        ">", "==", "count", "length", "size", "key?", "has_key?", "member?",
        "value?", "has_value?", "collect", "filter", "find_all", "alpha",
        "beta", "gamma", "missing"}) {
    ensure_symbol_id(&module, symbol);
  }
  append_string(&module, "beta");
  append_string(&module, "delta");
  const std::uint32_t one = append_integer_const(&module, 1);
  const std::uint32_t alpha = append_symbol_const(&module, "alpha");
  const std::uint32_t beta = append_symbol_const(&module, "beta");
  const std::uint32_t gamma = append_symbol_const(&module, "gamma");

  module.code_objects.push_back(
      make_send_code(1, symbol_id_or_die(module, "keys"), false));
  module.code_objects.push_back(
      make_send_code(2, symbol_id_or_die(module, "values"), false));
  module.code_objects.push_back(
      make_send_code(3, symbol_id_or_die(module, "entries"), false));
  module.code_objects.push_back(
      make_send_code(4, symbol_id_or_die(module, "to_a"), false));
  module.code_objects.push_back(
      make_send_code(5, symbol_id_or_die(module, "each"), true));
  module.code_objects.push_back(
      make_send_code(6, symbol_id_or_die(module, "map"), true));
  module.code_objects.push_back(
      make_send_code(7, symbol_id_or_die(module, "select"), true));
  module.code_objects.push_back(
      make_send_code(8, symbol_id_or_die(module, "reject"), true));
  module.code_objects.push_back(
      make_send_code(9, symbol_id_or_die(module, "transform_values"), true));
  module.code_objects.push_back(
      make_unary_send_code(10, symbol_id_or_die(module, "[]")));
  module.code_objects.push_back(
      make_send_code(11, symbol_id_or_die(module, "transform"), true));
  module.code_objects.push_back(
      make_unary_send_code(12, symbol_id_or_die(module, "contains?")));
  module.code_objects.push_back(
      make_unary_send_code(13, symbol_id_or_die(module, "include?")));
  module.code_objects.push_back(
      make_send_code(14, symbol_id_or_die(module, "count"), false));
  module.code_objects.push_back(
      make_send_code(15, symbol_id_or_die(module, "length"), false));
  module.code_objects.push_back(
      make_send_code(16, symbol_id_or_die(module, "size"), false));
  module.code_objects.push_back(
      make_unary_send_code(17, symbol_id_or_die(module, "key?")));
  module.code_objects.push_back(
      make_unary_send_code(18, symbol_id_or_die(module, "has_key?")));
  module.code_objects.push_back(
      make_unary_send_code(19, symbol_id_or_die(module, "member?")));
  module.code_objects.push_back(
      make_unary_send_code(20, symbol_id_or_die(module, "value?")));
  module.code_objects.push_back(
      make_unary_send_code(21, symbol_id_or_die(module, "has_value?")));
  module.code_objects.push_back(
      make_send_code(22, symbol_id_or_die(module, "collect"), true));
  module.code_objects.push_back(
      make_send_code(23, symbol_id_or_die(module, "filter"), true));
  module.code_objects.push_back(
      make_send_code(24, symbol_id_or_die(module, "find_all"), true));

  BcCode value_gt_one;
  value_gt_one.code_id = 100;
  value_gt_one.kind = CodeKind::Block;
  value_gt_one.reg_count = 4;
  value_gt_one.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {one, false}}});
  value_gt_one.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, ">"), {2}));
  value_gt_one.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(value_gt_one);

  BcCode inc_value;
  inc_value.code_id = 101;
  inc_value.kind = CodeKind::Block;
  inc_value.reg_count = 3;
  inc_value.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {one, false}}});
  inc_value.instructions.push_back(
      send_instr(2, 0, symbol_id_or_die(module, "+"), {1}));
  inc_value.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(inc_value);

  BcCode map_value_plus_one;
  map_value_plus_one.code_id = 102;
  map_value_plus_one.kind = CodeKind::Block;
  map_value_plus_one.reg_count = 4;
  map_value_plus_one.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {one, false}}});
  map_value_plus_one.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, "+"), {2}));
  map_value_plus_one.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(map_value_plus_one);

  BcCode key_eq_alpha;
  key_eq_alpha.code_id = 103;
  key_eq_alpha.kind = CodeKind::Block;
  key_eq_alpha.reg_count = 4;
  key_eq_alpha.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {alpha, false}}});
  key_eq_alpha.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "=="), {2}));
  key_eq_alpha.instructions.push_back({Opcode::Return, {{3, false}}});
  module.code_objects.push_back(key_eq_alpha);

  BcCode key_value_tuple;
  key_value_tuple.code_id = 104;
  key_value_tuple.kind = CodeKind::Block;
  key_value_tuple.reg_count = 3;
  key_value_tuple.instructions.push_back(
      {Opcode::MakeTuple, {{2, false}, {0, false}, {2, false}}});
  key_value_tuple.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(key_value_tuple);

  BcCode transform_rekey;
  transform_rekey.code_id = 105;
  transform_rekey.kind = CodeKind::Block;
  transform_rekey.reg_count = 5;
  transform_rekey.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {alpha, false}}});
  transform_rekey.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "=="), {2}));
  transform_rekey.instructions.push_back(
      {Opcode::JumpIfFalse, {{3, false}, {5, false}}});
  transform_rekey.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {beta, false}}});
  transform_rekey.instructions.push_back({Opcode::Jump, {{6, false}}});
  transform_rekey.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {gamma, false}}});
  transform_rekey.instructions.push_back(
      {Opcode::LoadK, {{3, false}, {one, false}}});
  transform_rekey.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, "+"), {3}));
  transform_rekey.instructions.push_back(
      {Opcode::MakeTuple, {{4, false}, {2, false}, {2, false}}});
  transform_rekey.instructions.push_back({Opcode::Return, {{4, false}}});
  module.code_objects.push_back(transform_rekey);

  BcCode transform_value_with_key;
  transform_value_with_key.code_id = 106;
  transform_value_with_key.kind = CodeKind::Block;
  transform_value_with_key.reg_count = 4;
  transform_value_with_key.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {alpha, false}}});
  transform_value_with_key.instructions.push_back(
      send_instr(3, 1, symbol_id_or_die(module, "=="), {2}));
  transform_value_with_key.instructions.push_back(
      {Opcode::JumpIfFalse, {{3, false}, {6, false}}});
  transform_value_with_key.instructions.push_back(
      {Opcode::LoadK, {{2, false}, {one, false}}});
  transform_value_with_key.instructions.push_back(
      send_instr(3, 0, symbol_id_or_die(module, "+"), {2}));
  transform_value_with_key.instructions.push_back(
      {Opcode::Return, {{3, false}}});
  transform_value_with_key.instructions.push_back(
      {Opcode::Return, {{0, false}}});
  module.code_objects.push_back(transform_value_with_key);

  return module;
}

void test_std001_map_protocol_matrix() {
  const amber::bytecode::BcModule module = make_map_protocol_module();
  const amber::runtime::Value map =
      make_symbol_map(module, {{"alpha", amber::runtime::Value::integer(1)},
                               {"beta", amber::runtime::Value::integer(2)}});
  const amber::runtime::Value value_gt_one = make_closure_value(100);
  const amber::runtime::Value inc_value = make_closure_value(101);
  const amber::runtime::Value map_value_plus_one = make_closure_value(102);
  const amber::runtime::Value key_eq_alpha = make_closure_value(103);
  const amber::runtime::Value key_value_tuple = make_closure_value(104);
  const amber::runtime::Value transform_rekey = make_closure_value(105);
  const amber::runtime::Value transform_value_with_key =
      make_closure_value(106);
  amber::runtime::Value string_key_map =
      amber::runtime::make_symbol_map_value(
          std::vector<amber::runtime::MapEntry>{
              amber::runtime::MapEntry{
                  amber::runtime::Value::string(
                      string_id_or_die(module, "beta")),
                  amber::runtime::Value::integer(2)}});

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 1, {map});
  expect_ok(result, "Map#keys");
  expect_symbol_list(module, result.value, {"alpha", "beta"}, "Map#keys");

  result = amber::runtime::execute_code(module, 2, {map});
  expect_ok(result, "Map#values");
  expect_integer_list(result.value, {1, 2}, "Map#values");

  result = amber::runtime::execute_code(module, 3, {map});
  expect_ok(result, "Map#entries");
  expect_entry_list(module, result.value, {{"alpha", 1}, {"beta", 2}},
                    "Map#entries");

  result = amber::runtime::execute_code(module, 4, {map});
  expect_ok(result, "Map#to_a");
  expect_entry_list(module, result.value, {{"alpha", 1}, {"beta", 2}},
                    "Map#to_a");

  result = amber::runtime::execute_code(
      module, 10,
      {map, amber::runtime::Value::symbol(symbol_id_or_die(module, "alpha"))});
  expect_ok(result, "Map#[] symbol key");
  expect_integer(result.value, 1, "Map#[] symbol key");

  result = amber::runtime::execute_code(
      module, 10,
      {map, amber::runtime::Value::string(string_id_or_die(module, "beta"))});
  expect_fault(result, "KeyError", "Map#[] string key is distinct from Symbol");

  result = amber::runtime::execute_code(
      module, 10,
      {string_key_map,
       amber::runtime::Value::string(string_id_or_die(module, "beta"))});
  expect_ok(result, "Map#[] stored string key");
  expect_integer(result.value, 2, "Map#[] stored string key");

  result = amber::runtime::execute_code(
      module, 10,
      {map, amber::runtime::Value::symbol(symbol_id_or_die(module, "missing"))});
  expect_fault(result, "KeyError", "Map#[] missing key");

  result = amber::runtime::execute_code(module, 5, {map, map_value_plus_one});
  expect_ok(result, "Map#each");
  expect(result.value.is_map() && result.value.as_map() == map.as_map(),
         "Map#each should return receiver");

  result = amber::runtime::execute_code(module, 6, {map, map_value_plus_one});
  expect_ok(result, "Map#map");
  expect_integer_list(result.value, {2, 3}, "Map#map");

  result = amber::runtime::execute_code(module, 6, {map, key_value_tuple});
  expect_ok(result, "Map#map key/value args");
  expect_entry_list(module, result.value, {{"alpha", 1}, {"beta", 2}},
                    "Map#map key/value args");

  result = amber::runtime::execute_code(module, 7, {map, value_gt_one});
  expect_ok(result, "Map#select");
  expect_symbol_map_entries(module, result.value, {{"beta", 2}},
                            "Map#select");

  result = amber::runtime::execute_code(module, 7, {map, key_eq_alpha});
  expect_ok(result, "Map#select key/value args");
  expect_symbol_map_entries(module, result.value, {{"alpha", 1}},
                            "Map#select key/value args");

  result = amber::runtime::execute_code(module, 8, {map, value_gt_one});
  expect_ok(result, "Map#reject");
  expect_symbol_map_entries(module, result.value, {{"alpha", 1}},
                            "Map#reject");

  result = amber::runtime::execute_code(module, 8, {map, key_eq_alpha});
  expect_ok(result, "Map#reject key/value args");
  expect_symbol_map_entries(module, result.value, {{"beta", 2}},
                            "Map#reject key/value args");

  result = amber::runtime::execute_code(module, 9, {map, inc_value});
  expect_ok(result, "Map#transform_values");
  expect_symbol_map_entries(module, result.value, {{"alpha", 2}, {"beta", 3}},
                            "Map#transform_values");

  result = amber::runtime::execute_code(module, 9,
                                        {map, transform_value_with_key});
  expect_ok(result, "Map#transform_values key-aware");
  expect_symbol_map_entries(module, result.value, {{"alpha", 2}, {"beta", 2}},
                            "Map#transform_values key-aware");

  result = amber::runtime::execute_code(module, 11, {map, transform_rekey});
  expect_ok(result, "Map#transform");
  expect_symbol_map_entries(module, result.value, {{"beta", 2}, {"gamma", 3}},
                            "Map#transform");

  result = amber::runtime::execute_code(module, 14, {map});
  expect_ok(result, "Map#count");
  expect_integer(result.value, 2, "Map#count");

  result = amber::runtime::execute_code(module, 15, {map});
  expect_ok(result, "Map#length");
  expect_integer(result.value, 2, "Map#length");

  result = amber::runtime::execute_code(module, 16, {map});
  expect_ok(result, "Map#size");
  expect_integer(result.value, 2, "Map#size");

  result = amber::runtime::execute_code(
      module, 17,
      {map, amber::runtime::Value::symbol(symbol_id_or_die(module, "alpha"))});
  expect_ok(result, "Map#key?");
  expect_bool(result.value, true, "Map#key?");

  result = amber::runtime::execute_code(
      module, 18,
      {map, amber::runtime::Value::symbol(symbol_id_or_die(module, "alpha"))});
  expect_ok(result, "Map#has_key?");
  expect_bool(result.value, true, "Map#has_key?");

  result = amber::runtime::execute_code(
      module, 19,
      {map, amber::runtime::Value::symbol(symbol_id_or_die(module, "alpha"))});
  expect_ok(result, "Map#member?");
  expect_bool(result.value, true, "Map#member?");

  result = amber::runtime::execute_code(
      module, 20, {map, amber::runtime::Value::integer(2)});
  expect_ok(result, "Map#value?");
  expect_bool(result.value, true, "Map#value?");

  result = amber::runtime::execute_code(
      module, 21, {map, amber::runtime::Value::integer(2)});
  expect_ok(result, "Map#has_value?");
  expect_bool(result.value, true, "Map#has_value?");

  result = amber::runtime::execute_code(module, 22, {map, map_value_plus_one});
  expect_ok(result, "Map#collect alias");
  expect_integer_list(result.value, {2, 3}, "Map#collect alias");

  result = amber::runtime::execute_code(module, 23, {map, value_gt_one});
  expect_ok(result, "Map#filter alias");
  expect_symbol_map_entries(module, result.value, {{"beta", 2}},
                            "Map#filter alias");

  result = amber::runtime::execute_code(module, 24, {map, value_gt_one});
  expect_ok(result, "Map#find_all alias");
  expect_symbol_map_entries(module, result.value, {{"beta", 2}},
                            "Map#find_all alias");
}

void test_std001_empty_map_edges() {
  const amber::bytecode::BcModule module = make_map_protocol_module();
  const amber::runtime::Value empty = amber::runtime::make_symbol_map_value({});
  const amber::runtime::Value value_gt_one = make_closure_value(100);
  const amber::runtime::Value inc_value = make_closure_value(101);
  const amber::runtime::Value map_value_plus_one = make_closure_value(102);
  const amber::runtime::Value transform_rekey = make_closure_value(105);

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 1, {empty});
  expect_ok(result, "empty Map#keys");
  expect_integer_list(result.value, {}, "empty Map#keys");

  result = amber::runtime::execute_code(module, 2, {empty});
  expect_ok(result, "empty Map#values");
  expect_integer_list(result.value, {}, "empty Map#values");

  result = amber::runtime::execute_code(module, 3, {empty});
  expect_ok(result, "empty Map#entries");
  expect_integer_list(result.value, {}, "empty Map#entries");

  result = amber::runtime::execute_code(module, 14, {empty});
  expect_ok(result, "empty Map#count");
  expect_integer(result.value, 0, "empty Map#count");

  result = amber::runtime::execute_code(module, 15, {empty});
  expect_ok(result, "empty Map#length");
  expect_integer(result.value, 0, "empty Map#length");

  result = amber::runtime::execute_code(module, 16, {empty});
  expect_ok(result, "empty Map#size");
  expect_integer(result.value, 0, "empty Map#size");

  result = amber::runtime::execute_code(module, 5, {empty, map_value_plus_one});
  expect_ok(result, "empty Map#each");
  expect(result.value.is_map() && result.value.as_map() == empty.as_map(),
         "empty Map#each should return receiver");

  result = amber::runtime::execute_code(module, 6, {empty, map_value_plus_one});
  expect_ok(result, "empty Map#map");
  expect_integer_list(result.value, {}, "empty Map#map");

  result = amber::runtime::execute_code(module, 7, {empty, value_gt_one});
  expect_ok(result, "empty Map#select");
  expect(result.value.is_map() && result.value.as_map()->entries.empty(),
         "empty Map#select should return empty map");

  result = amber::runtime::execute_code(module, 8, {empty, value_gt_one});
  expect_ok(result, "empty Map#reject");
  expect(result.value.is_map() && result.value.as_map()->entries.empty(),
         "empty Map#reject should return empty map");

  result = amber::runtime::execute_code(module, 9, {empty, inc_value});
  expect_ok(result, "empty Map#transform_values");
  expect(result.value.is_map() && result.value.as_map()->entries.empty(),
         "empty Map#transform_values should return empty map");

  result = amber::runtime::execute_code(module, 11, {empty, transform_rekey});
  expect_ok(result, "empty Map#transform");
  expect(result.value.is_map() && result.value.as_map()->entries.empty(),
         "empty Map#transform should return empty map");
}

void test_std006_collection_error_edges() {
  const amber::bytecode::BcModule sequence_module =
      make_sequence_protocol_module();
  auto integer = [](std::int64_t value) {
    return amber::runtime::Value::integer(value);
  };

  const amber::runtime::Value list =
      amber::runtime::make_list_value({integer(1), integer(2), integer(3),
                                       integer(4), integer(5)});
  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(sequence_module, 24, {list, integer(-1)});
  expect_ok(result, "Array#[] negative index");
  expect_integer(result.value, 5, "Array#[] negative index");

  result =
      amber::runtime::execute_code(sequence_module, 24, {list, integer(99)});
  expect_fault(result, "IndexError", "Array#[] out of bounds");

  const amber::runtime::Value tail_slice =
      make_range_value(sequence_module, -3, -1);
  result =
      amber::runtime::execute_code(sequence_module, 24, {list, tail_slice});
  expect_ok(result, "Array#[] negative endpoint slice");
  expect_integer_list(result.value, {3, 4, 5},
                      "Array#[] negative endpoint slice");

  amber::runtime::Value open_slice =
      make_range_value(sequence_module, 2, 0, true);
  open_slice.as_instance_object()->ivars["finish"] =
      amber::runtime::Value::null();
  result =
      amber::runtime::execute_code(sequence_module, 24, {list, open_slice});
  expect_ok(result, "Array#[] open-ended slice");
  expect_integer_list(result.value, {3, 4, 5}, "Array#[] open-ended slice");

  const amber::runtime::Value stepped_slice =
      make_range_value(sequence_module, 4, 0, true, -2);
  result =
      amber::runtime::execute_code(sequence_module, 24, {list, stepped_slice});
  expect_ok(result, "Array#[] stepped descending slice");
  expect_integer_list(result.value, {5, 3, 1},
                      "Array#[] stepped descending slice");

  const amber::runtime::Value range = make_range_value(sequence_module, 1, 3);
  result =
      amber::runtime::execute_code(sequence_module, 24, {range, integer(9)});
  expect_fault(result, "IndexError", "Range#[] out of bounds");

  amber::runtime::Value open_end =
      make_range_value(sequence_module, 4, 0, true);
  open_end.as_instance_object()->ivars["finish"] =
      amber::runtime::Value::null();
  result = amber::runtime::execute_code(sequence_module, 24,
                                        {open_end, integer(-1)});
  expect_fault(result, "IndexError", "open-ended Range#[] negative index");

  result = amber::runtime::execute_code(sequence_module, 25,
                                        {list, make_closure_value(100)});
  expect_ok(result, "LazySeq construction for error edges");
  const amber::runtime::Value lazy = result.value;
  result =
      amber::runtime::execute_code(sequence_module, 24, {lazy, integer(99)});
  expect_fault(result, "IndexError", "LazySeq#[] out of bounds");

  const amber::bytecode::BcModule map_module = make_map_protocol_module();
  const amber::runtime::Value map =
      make_symbol_map(map_module, {{"alpha", integer(1)},
                                   {"beta", integer(2)}});

  result = amber::runtime::execute_code(
      map_module, 10,
      {map,
       amber::runtime::Value::symbol(symbol_id_or_die(map_module, "missing"))});
  expect_fault(result, "KeyError", "Map#[] missing symbol key");

  result = amber::runtime::execute_code(
      map_module, 10,
      {map, amber::runtime::Value::string(string_id_or_die(map_module,
                                                           "delta"))});
  expect_fault(result, "KeyError", "Map#[] missing string key");

  result = amber::runtime::execute_code(
      map_module, 12,
      {map,
       amber::runtime::Value::symbol(symbol_id_or_die(map_module, "alpha"))});
  expect_ok(result, "Map#contains? present key");
  expect_bool(result.value, true, "Map#contains? present key");

  result = amber::runtime::execute_code(
      map_module, 13,
      {map,
       amber::runtime::Value::symbol(symbol_id_or_die(map_module, "missing"))});
  expect_ok(result, "Map#include? missing key");
  expect_bool(result.value, false, "Map#include? missing key");

  result = amber::runtime::execute_code(map_module, 10, {map, integer(1)});
  expect_fault(result, "KeyError", "Map#[] missing integer key");
}

amber::bytecode::BcModule make_collection_block_edge_module() {
  using namespace amber::bytecode;

  BcModule module;
  for (const std::string &symbol : {"each", "map", "Boom"}) {
    ensure_symbol_id(&module, symbol);
  }
  const std::uint32_t boom = append_symbol_const(&module, "Boom");

  module.code_objects.push_back(
      make_send_code(1, symbol_id_or_die(module, "map"), true));
  module.code_objects.push_back(
      make_send_code(2, symbol_id_or_die(module, "each"), true));

  BcCode raise_boom;
  raise_boom.code_id = 100;
  raise_boom.kind = CodeKind::Block;
  raise_boom.reg_count = 2;
  raise_boom.instructions.push_back(
      {Opcode::LoadK, {{1, false}, {boom, false}}});
  raise_boom.instructions.push_back({Opcode::Raise, {{1, false}}});
  raise_boom.instructions.push_back({Opcode::Return, {{1, false}}});
  module.code_objects.push_back(raise_boom);

  BcCode destroy_capture;
  destroy_capture.code_id = 101;
  destroy_capture.kind = CodeKind::Block;
  destroy_capture.reg_count = 3;
  destroy_capture.instructions.push_back(
      {Opcode::LoadUpval, {{1, false}, {0, false}}});
  destroy_capture.instructions.push_back(
      {Opcode::ObjDestroy, {{2, false}, {1, false}}});
  destroy_capture.instructions.push_back({Opcode::Return, {{2, false}}});
  module.code_objects.push_back(destroy_capture);

  return module;
}

void test_std001_block_exception_propagation() {
  const amber::bytecode::BcModule module = make_collection_block_edge_module();
  const amber::runtime::Value source =
      amber::runtime::make_list_value({amber::runtime::Value::integer(1)});

  const amber::runtime::ExecutionResult result = amber::runtime::execute_code(
      module, 1, {source, make_closure_value(100)});
  expect_fault(result, "Boom", "collection block exception propagation");
}

void test_std001_mutation_during_iteration_edges() {
  const amber::bytecode::BcModule module = make_collection_block_edge_module();

  amber::runtime::Value list = amber::runtime::make_list_value(
      {amber::runtime::Value::integer(1), amber::runtime::Value::integer(2)});
  amber::runtime::ExecutionResult result = amber::runtime::execute_code(
      module, 2, {list, make_closure_value(101, {list})});
  expect_fault(result, "DestroyedAccessError",
               "Array mutation during iteration");

  amber::runtime::Value map = amber::runtime::make_symbol_map_value(
      {{symbol_id_or_die(module, "Boom"), amber::runtime::Value::integer(1)}});
  result = amber::runtime::execute_code(module, 2,
                                        {map, make_closure_value(101, {map})});
  expect_fault(result, "DestroyedAccessError", "Map mutation during iteration");
}

} // namespace

int main() {
  test_std001_sequence_protocol_matrix();
  test_std002_range_eager_methods();
  test_std001_empty_sequence_edges();
  test_std002_empty_range_edges();
  test_std002_range_exclusive_and_open_end_edges();
  test_std002_int_times();
  test_std003_lazy_pipeline_and_materialization();
  test_std005_collection_operations();
  test_std001_map_protocol_matrix();
  test_std001_empty_map_edges();
  test_std006_collection_error_edges();
  test_std001_block_exception_propagation();
  test_std001_mutation_during_iteration_edges();
  std::cout << "stdlib_collections_tests: ok\n";
  return 0;
}
