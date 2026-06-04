#define AMBER_IAMBER_TESTING
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "tools/iamber/main.cpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
  }
}

std::string output_text(
    const std::vector<amber::runtime::RuntimeTextOutputEvent> &events) {
  std::string out;
  for (const amber::runtime::RuntimeTextOutputEvent &event : events) {
    out += event.text;
  }
  return out;
}

Cell make_cell(std::string source) {
  Cell cell;
  cell.source = std::move(source);
  return cell;
}

void test_prefix_eval_filters_prior_cell_output() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("logger = io.Logger.new\n"
                            "logger.info(\"first-cell\")\n"
                            "1\n"));
  cells.push_back(make_cell("logger\n"));

  const EvalView first = evaluate_prefix(cells, 0);
  expect(first.ok, "first cell should evaluate");
  expect(output_text(first.output_events).find("first-cell") !=
             std::string::npos,
         "first cell should keep its own logger output");

  const EvalView second = evaluate_prefix(cells, 1);
  expect(second.ok, "second cell should evaluate");
  expect(output_text(second.output_events).find("first-cell") ==
             std::string::npos,
         "second cell should not inherit prior cell logger output");
}

void test_prefix_eval_keeps_current_cell_output() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("x = 1\n"));
  cells.push_back(make_cell("logger = io.Logger.new\n"
                            "logger.warn(\"second-cell\")\n"
                            "Array\n"));

  const EvalView second = evaluate_prefix(cells, 1);
  expect(second.ok, "second cell with logger should evaluate");
  expect(output_text(second.output_events).find("second-cell") !=
             std::string::npos,
         "second cell should keep its own logger output");
}

void test_independent_cell_uses_isolated_eval() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("1 / 0\n"));
  cells.push_back(make_cell("Array\n"));

  const EvalView second = evaluate_prefix(cells, 1);
  expect(second.ok, "independent second cell should not execute first cell");
}

void test_compound_assignment_eval() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("x = 2\n"
                            "x += 1\n"));

  const EvalView view = evaluate_prefix(cells, 0);
  expect(view.ok, "compound assignment cell should evaluate");
  expect(view.result == "3", "compound assignment should return updated value");
}

void test_range_literal_uses_native_prelude() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("(0..5)\n"));

  EvalView view = evaluate_prefix(cells, 0);
  expect(view.ok, "range literal should evaluate without a local Range class");
  expect(view.result == "<instance Range>",
         "range literal should use the native Range prelude");

  cells[0] = make_cell("(0..5).array\n");
  view = evaluate_prefix(cells, 0);
  expect(view.ok, "native Range literal should materialize");
  expect(view.result == "[0, 1, 2, 3, 4, 5]",
         "range literal should materialize through the native prelude");
}

void test_cell_can_read_binding_initialized_earlier_in_cell() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("h = {}\n"
                            "h[?:key]\n"));

  expect(!cyclic_watch_error_for_cell(cells, 0).has_value(),
         "same-cell read after initialization should not be cyclic watch");

  const EvalView view = evaluate_prefix(cells, 0);
  expect(view.ok, "same-cell optional map lookup should evaluate");
  expect(view.result == "null", "missing optional map key should return null");
}

void test_cyclic_watch_dependency_blocks_self_write() {
  std::vector<Cell> cells;
  cells.push_back(make_cell("x = 2\n"));
  cells.push_back(make_cell("x = 6 + x\n"));
  cells.push_back(make_cell("x += 1\n"));

  std::optional<std::string> second_cycle =
      cyclic_watch_error_for_cell(cells, 1);
  expect(second_cycle.has_value(),
         "self read/write assignment should be cyclic watch");
  expect(second_cycle->find("reads and writes x") != std::string::npos,
         "cycle message should name binding");

  std::optional<std::string> third_cycle =
      cyclic_watch_error_for_cell(cells, 2);
  expect(third_cycle.has_value(),
         "compound assignment should be cyclic watch");

  cells[1].watch = false;
  expect(!cyclic_watch_error_for_cell(cells, 1).has_value(),
         "disabled watch cell should not be blocked as cycle");
}

void test_evaluate_from_blocks_cyclic_watch_cell() {
  Session session;
  session.cells.push_back(make_cell("x = 2\n"));
  session.cells.push_back(make_cell("x = 6 + x\n"));
  session.cells.push_back(make_cell("x += 1\n"));

  evaluate_from(&session, 0, false, false, false);
  expect(session.cells[0].ok, "acyclic first cell should evaluate");
  expect(!session.cells[1].ok, "cyclic second cell should fail");
  expect(session.cells[1].error.find("cyclic watch dependency") !=
             std::string::npos,
         "cyclic second cell should report watch cycle");
  expect(!session.cells[2].ok, "cyclic compound cell should fail");
  expect(session.cells[2].error.find("cyclic watch dependency") !=
             std::string::npos,
         "cyclic compound cell should report watch cycle");
}

} // namespace

int main() {
  test_prefix_eval_filters_prior_cell_output();
  test_prefix_eval_keeps_current_cell_output();
  test_independent_cell_uses_isolated_eval();
  test_compound_assignment_eval();
  test_range_literal_uses_native_prelude();
  test_cell_can_read_binding_initialized_earlier_in_cell();
  test_cyclic_watch_dependency_blocks_self_write();
  test_evaluate_from_blocks_cyclic_watch_cell();
  std::cout << "iamber_tests ok\n";
  return 0;
}
