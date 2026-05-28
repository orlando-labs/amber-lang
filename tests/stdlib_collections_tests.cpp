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
           std::int64_t block_reg = -1) {
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

amber::runtime::Value
make_closure_value(std::uint32_t code_id,
                   std::vector<amber::runtime::Value> captures = {}) {
  auto closure = std::make_shared<amber::runtime::ClosureValue>();
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
  const std::shared_ptr<amber::runtime::ListValue> list = value.as_list();
  expect(list != nullptr, message + " list payload");
  expect(list->items.size() == expected.size(), message + " list size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect_integer(list->items[i], expected[i],
                   message + " item " + std::to_string(i));
  }
}

void expect_symbol_list(const amber::bytecode::BcModule &module,
                        const amber::runtime::Value &value,
                        const std::vector<std::string> &expected,
                        const std::string &message) {
  expect(value.is_list(), message + " should be a list");
  const std::shared_ptr<amber::runtime::ListValue> list = value.as_list();
  expect(list != nullptr, message + " list payload");
  expect(list->items.size() == expected.size(), message + " list size");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect(list->items[i].is_symbol(), message + " item should be symbol");
    expect(list->items[i].as_symbol().symbol_id ==
               symbol_id_or_die(module, expected[i]),
           message + " item " + std::to_string(i));
  }
}

amber::bytecode::BcModule make_sequence_protocol_module() {
  using namespace amber::bytecode;

  BcModule module;
  for (const std::string &symbol :
       {"each", "map", "flat_map", "select", "reject", "reduce", "find", "any?",
        "all?", "none?", "first", "count", "to_a", "lazy", "group_by", "+", ">",
        "low", "high"}) {
    ensure_symbol_id(&module, symbol);
  }
  const std::uint32_t zero = append_integer_const(&module, 0);
  const std::uint32_t one = append_integer_const(&module, 1);
  const std::uint32_t three = append_integer_const(&module, 3);
  const std::uint32_t low = append_symbol_const(&module, "low");
  const std::uint32_t high = append_symbol_const(&module, "high");

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
  } else {
    expect(result.value.is_tuple() &&
               result.value.as_tuple() == source.as_tuple(),
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
  const std::shared_ptr<amber::runtime::MapValue> groups =
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
}

void test_std001_empty_sequence_edges() {
  const amber::bytecode::BcModule module = make_sequence_protocol_module();
  const amber::runtime::Value add = make_closure_value(102);

  for (const auto &entry : {std::pair<std::string, amber::runtime::Value>{
                                "Array", amber::runtime::make_list_value({})},
                            {"Tuple", amber::runtime::make_tuple_value({})}}) {
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

amber::bytecode::BcModule make_map_protocol_module() {
  using namespace amber::bytecode;

  BcModule module;
  for (const std::string &symbol :
       {"keys", "values", "entries", "to_a", "each", "map", "select", "reject",
        "transform_values", "+", ">", "alpha", "beta"}) {
    ensure_symbol_id(&module, symbol);
  }
  const std::uint32_t one = append_integer_const(&module, 1);

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
  inc_value.instructions.push_back({Opcode::LoadK, {{1, false}, {one, false}}});
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

  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, 1, {map});
  expect_ok(result, "Map#keys");
  expect_symbol_list(module, result.value, {"alpha", "beta"}, "Map#keys");

  result = amber::runtime::execute_code(module, 2, {map});
  expect_ok(result, "Map#values");
  expect_integer_list(result.value, {1, 2}, "Map#values");

  result = amber::runtime::execute_code(module, 3, {map});
  expect_ok(result, "Map#entries");
  expect(result.value.is_list(), "Map#entries should return list");
  const std::shared_ptr<amber::runtime::ListValue> entries =
      result.value.as_list();
  expect(entries != nullptr && entries->items.size() == 2, "Map#entries shape");
  expect(entries->items[0].is_tuple() && entries->items[1].is_tuple(),
         "Map#entries tuple entries");

  result = amber::runtime::execute_code(module, 4, {map});
  expect_ok(result, "Map#to_a");
  expect(result.value.is_list() && result.value.as_list()->items.size() == 2,
         "Map#to_a entries");

  result = amber::runtime::execute_code(module, 5, {map, map_value_plus_one});
  expect_ok(result, "Map#each");
  expect(result.value.is_map() && result.value.as_map() == map.as_map(),
         "Map#each should return receiver");

  result = amber::runtime::execute_code(module, 6, {map, map_value_plus_one});
  expect_ok(result, "Map#map");
  expect_integer_list(result.value, {2, 3}, "Map#map");

  result = amber::runtime::execute_code(module, 7, {map, value_gt_one});
  expect_ok(result, "Map#select");
  expect(result.value.is_map(), "Map#select should return map");
  expect(result.value.as_map()->entries.size() == 1 &&
             result.value.as_map()->entries[0].symbol_id ==
                 symbol_id_or_die(module, "beta"),
         "Map#select should preserve matching entries");

  result = amber::runtime::execute_code(module, 8, {map, value_gt_one});
  expect_ok(result, "Map#reject");
  expect(result.value.is_map(), "Map#reject should return map");
  expect(result.value.as_map()->entries.size() == 1 &&
             result.value.as_map()->entries[0].symbol_id ==
                 symbol_id_or_die(module, "alpha"),
         "Map#reject should preserve non-matching entries");

  result = amber::runtime::execute_code(module, 9, {map, inc_value});
  expect_ok(result, "Map#transform_values");
  expect(result.value.is_map(), "Map#transform_values should return map");
  expect(result.value.as_map()->entries.size() == 2,
         "Map#transform_values shape");
  expect_integer(result.value.as_map()->entries[0].value, 2,
                 "Map#transform_values alpha");
  expect_integer(result.value.as_map()->entries[1].value, 3,
                 "Map#transform_values beta");
}

void test_std001_empty_map_edges() {
  const amber::bytecode::BcModule module = make_map_protocol_module();
  const amber::runtime::Value empty = amber::runtime::make_symbol_map_value({});
  const amber::runtime::Value value_gt_one = make_closure_value(100);
  const amber::runtime::Value inc_value = make_closure_value(101);
  const amber::runtime::Value map_value_plus_one = make_closure_value(102);

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
  test_std001_empty_sequence_edges();
  test_std001_map_protocol_matrix();
  test_std001_empty_map_edges();
  test_std001_block_exception_propagation();
  test_std001_mutation_during_iteration_edges();
  std::cout << "stdlib_collections_tests: ok\n";
  return 0;
}
