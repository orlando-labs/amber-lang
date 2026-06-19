#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/vm.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib argparser test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-argparser-source-test>");
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
  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emit_result.module));
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute_source(const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_ok_integer(const amber::runtime::ExecutionResult &result,
                       std::int64_t expected, const std::string &message) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << message << ": " << result.fault->error_name
              << " / " << result.fault->message << "\n";
  }
  expect(result.ok(), message + " should succeed");
  expect(result.value.is_integer(), message + " should return Int");
  expect(result.value.as_integer() == expected, message + " value");
}

void expect_fault(const std::string &source, const std::string &error_name,
                  const std::string &message) {
  const amber::runtime::ExecutionResult result = execute_source(source);
  expect(!result.ok() && result.fault.has_value(), message + " should fault");
  expect(result.fault->error_name == error_name,
         message + " should fault with " + error_name + ", got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_options_positionals_and_rest() {
  const amber::runtime::ExecutionResult result = execute_source(
      "parser = ArgParser(cmdline: [\"--port\", \"8080\", "
      "\"--host=0.0.0.0\", \"-v\", \"src\", \"dst\", \"--\", "
      "\"--literal\"])\n"
      "parser.name(\"copy\")\n"
      "parser.arg(\"-p\", \"--port\", name: \"port\", type: Int, "
      "default: 3000)\n"
      "parser.arg(\"--host\", type: Str, default: \"127.0.0.1\")\n"
      "parser.flag(\"-v\", \"--verbose\")\n"
      "parser.pos(\"source\")\n"
      "parser.pos(\"target\")\n"
      "parser.rest(\"tail\")\n"
      "args = parser.parse_or_raise()\n"
      "if args[\"port\"] == 8080 and args[\"host\"] == \"0.0.0.0\" and "
      "args[\"verbose\"] == true and args[\"source\"] == \"src\" and "
      "args[\"target\"] == \"dst\" and args[\"tail\"][0] == \"--literal\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "ArgParser options, positionals, rest");
}

void test_result_mode_choices_and_help() {
  const amber::runtime::ExecutionResult result = execute_source(
      "bad_parser = ArgParser(cmdline: [\"--mode\", \"staging\"])\n"
      "bad_parser.arg(\"--mode\", choices: [\"dev\", \"prod\"])\n"
      "bad = bad_parser.try_parse()\n"
      "help_parser = ArgParser(cmdline: [\"--help\"])\n"
      "help_parser.name(\"copy\")\n"
      "help_parser.about(\"Copy files\")\n"
      "help_parser.flag(\"-v\", \"--verbose\")\n"
      "help = help_parser.try_parse()\n"
      "bad_error = bad.error()\n"
      "help_error = help.error()\n"
      "if bad.err?() and ArgParser.InvalidChoice === bad_error and "
      "bad_error.option == \"--mode\" and help.err?() and "
      "ArgParser.HelpRequested === help_error and help_error.exit_code == 0 "
      "and help_error.help.contains?(\"usage: copy\"):\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "ArgParser result mode, choices, help");
}

void test_env_multiple_negatable_and_defaults() {
  const amber::runtime::ExecutionResult result = execute_source(
      "env = {\"API_TOKEN\": \"sekret\"}\n"
      "parser = ArgParser(cmdline: [\"--include\", \"src\", \"-I\", "
      "\"lib\", \"--no-color\"], env: env)\n"
      "parser.arg(\"-I\", \"--include\", name: \"includes\", "
      "multiple: true)\n"
      "parser.arg(\"--token\", env: \"API_TOKEN\", required: true)\n"
      "parser.flag(\"--color\", default: true, negatable: true)\n"
      "args = parser.parse_or_raise()\n"
      "if args[\"includes\"][0] == \"src\" and "
      "args[\"includes\"][1] == \"lib\" and args[\"token\"] == "
      "\"sekret\" and args[\"color\"] == false:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42,
                    "ArgParser env fallback, multiple, negatable flags");
}

void test_cmdline_override_and_local_converter() {
  const amber::runtime::ExecutionResult result = execute_source(
      "parser = ArgParser(cmdline: [\"--port\", \"1\"])\n"
      "parser.arg(\"--port\", type: Int) |value|:\n"
      "  value + 1\n"
      "args = parser.parse_or_raise(cmdline: [\"--port\", \"41\"])\n"
      "result = parser.try_parse(cmdline: [\"--port\", \"5\"])\n"
      "if args[\"port\"] == 42 and result.ok?() and "
      "result.value()[\"port\"] == 6:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42,
                    "ArgParser cmdline override and local converter");
}

void test_parse_or_raise_faults() {
  expect_fault("parser = ArgParser(cmdline: [])\n"
               "parser.arg(\"--token\", required: true)\n"
               "parser.parse_or_raise()\n",
               "ArgParser.MissingRequired", "missing required option");
  expect_fault("ArgParser(cmdline: [\"--wat\"]).parse_or_raise()\n",
               "ArgParser.UnknownOption", "unknown option");
  expect_fault("parser = ArgParser(cmdline: [\"--count\", \"nope\"])\n"
               "parser.arg(\"--count\", type: Int)\n"
               "parser.parse_or_raise()\n",
               "ArgParser.InvalidValue", "invalid Int option");
  expect_fault("ArgParser(cmdline: [\"--help\"]).parse_or_raise()\n",
               "ArgParser.HelpRequested", "strict help request");
}

void test_first_class_errors_and_strict_equivalence() {
  const amber::runtime::ExecutionResult result = execute_source(
      "parser = ArgParser(cmdline: [\"--count\", \"bad\"])\n"
      "parser.arg(\"--count\", type: Int)\n"
      "result = parser.try_parse()\n"
      "error = result.error()\n"
      "from_result = result.err?() and ArgParser.InvalidValue === error and "
      "error.message == \"--count expects Int\" and "
      "error.option == \"--count\" and error.value == \"bad\" and "
      "error.exit_code == 2 and error.usage.contains?(\"usage: program\") "
      "and error.help == error.usage\n"
      "from_bridge = false\n"
      "try:\n"
      "  parser.try_parse().or_raise\n"
      "rescue ArgParser.InvalidValue |e|:\n"
      "  from_bridge = e.message == error.message and "
      "e.option == error.option and e.value == error.value and "
      "e.exit_code == error.exit_code\n"
      "from_shorthand = false\n"
      "try:\n"
      "  parser.parse_or_raise()\n"
      "rescue ArgParser.InvalidValue |e|:\n"
      "  from_shorthand = e.message == error.message and "
      "e.option == error.option and e.value == error.value and "
      "e.exit_code == error.exit_code\n"
      "if from_result and from_bridge and from_shorthand:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "ArgParser first-class strict equivalence");
}

void test_dotted_error_construction_and_matching() {
  const amber::runtime::ExecutionResult result = execute_source(
      "one = ArgParser.InvalidValue.new(\"bad\", option: \"--port\", "
      "value: \"nope\")\n"
      "two = ArgParser.InvalidValue(\"worse\", option: \"--count\", "
      "value: \"nah\", exit_code: 7)\n"
      "base = ArgParser.ParseError(\"base\", usage: \"usage: app\")\n"
      "help = ArgParser.HelpRequested()\n"
      "bridged = false\n"
      "try:\n"
      "  Err(two).or_raise\n"
      "rescue ArgParser.InvalidValue |e|:\n"
      "  bridged = e.exit_code == 7 and e.option == \"--count\"\n"
      "if ArgParser.InvalidValue === one and "
      "ArgParser.ParseError === one and "
      "(ArgParser.UnknownOption === one) == false and "
      "one.option == \"--port\" and "
      "one.value == \"nope\" and one.exit_code == 2 and "
      "ArgParser.InvalidValue === two and two.exit_code == 7 and "
      "ArgParser.ParseError === base and base.usage == \"usage: app\" and "
      "bridged and "
      "ArgParser.HelpRequested === help and help.message == "
      "\"help requested\" and help.exit_code == 0 and help.help == null and "
      "(ArgParser.ParseError === help) == false:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "ArgParser dotted error construction");
}

void test_parse_error_rescue_hierarchy() {
  const std::vector<std::string> cases = {
      "candidate = ArgParser(cmdline: [\"--wat\"])",
      "candidate = ArgParser(cmdline: [\"--port\"])\n"
      "candidate.arg(\"--port\")",
      "candidate = ArgParser(cmdline: [])\n"
      "candidate.arg(\"--token\", required: true)",
      "candidate = ArgParser(cmdline: [\"extra\"])",
      "candidate = ArgParser(cmdline: [\"--count\", \"bad\"])\n"
      "candidate.arg(\"--count\", type: Int)",
      "candidate = ArgParser(cmdline: [\"--mode\", \"bad\"])\n"
      "candidate.arg(\"--mode\", choices: [\"dev\"])",
  };
  for (const std::string &parser_source : cases) {
    const amber::runtime::ExecutionResult result =
        execute_source(parser_source + "\n"
                                       "try:\n"
                                       "  candidate.try_parse().or_raise\n"
                                       "rescue ArgParser.ParseError:\n"
                                       "  42\n");
    expect_ok_integer(result, 42,
                      "ArgParser.ParseError catches parse subclass");
  }

  const amber::runtime::ExecutionResult help = execute_source(
      "try:\n"
      "  try:\n"
      "    ArgParser(cmdline: [\"--help\"]).try_parse().or_raise\n"
      "  rescue ArgParser.ParseError:\n"
      "    0\n"
      "rescue ArgParser.HelpRequested:\n"
      "  42\n");
  expect_ok_integer(help, 42, "ArgParser.ParseError excludes HelpRequested");

  const amber::runtime::ExecutionResult sibling = execute_source(
      "try:\n"
      "  try:\n"
      "    ArgParser(cmdline: [\"--wat\"]).try_parse().or_raise\n"
      "  rescue ArgParser.InvalidValue:\n"
      "    0\n"
      "rescue ArgParser.UnknownOption:\n"
      "  42\n");
  expect_ok_integer(sibling, 42,
                    "ArgParser.InvalidValue excludes sibling errors");
}

void test_converter_exception_boundaries() {
  const amber::runtime::ExecutionResult parse_error = execute_source(
      "parser = ArgParser(cmdline: [\"--port\", \"9\"])\n"
      "parser.arg(\"--port\", type: Int) |value|:\n"
      "  raise ArgParser.InvalidValue(\"out of range\", option: \"--port\", "
      "value: value)\n"
      "result = parser.try_parse()\n"
      "error = result.error()\n"
      "if result.err?() and ArgParser.InvalidValue === error and "
      "error.message == \"out of range\" and error.option == \"--port\" and "
      "error.value == 9:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(parse_error, 42,
                    "ArgParser captures explicit converter parse errors");

  const amber::runtime::ExecutionResult arbitrary =
      execute_source("parser = ArgParser(cmdline: [\"--port\", \"9\"])\n"
                     "parser.arg(\"--port\", type: Int) |value|:\n"
                     "  raise ValueError(\"converter exploded\")\n"
                     "try:\n"
                     "  parser.try_parse()\n"
                     "rescue ValueError |e|:\n"
                     "  if e.message == \"converter exploded\":\n"
                     "    42\n"
                     "  else:\n"
                     "    0\n");
  expect_ok_integer(arbitrary, 42,
                    "ArgParser preserves arbitrary converter exceptions");
}

void test_unhandled_error_name_and_trace() {
  const amber::runtime::ExecutionResult result =
      execute_source("parser = ArgParser(cmdline: [\"--count\", \"bad\"])\n"
                     "parser.arg(\"--count\", type: Int)\n"
                     "parser.try_parse().or_raise\n");
  expect(!result.ok() && result.fault.has_value(),
         "unhandled ArgParser error should fault");
  expect(result.fault->error_name == "ArgParser.InvalidValue",
         "unhandled ArgParser error keeps dotted name");
  expect(!result.fault->trace.empty(),
         "unhandled ArgParser error keeps source trace");
  expect(result.fault->trace_text.find("ArgParser.InvalidValue:") !=
             std::string::npos,
         "unhandled ArgParser error trace text keeps dotted name");
}

void test_parse_cli_behavior() {
  const std::shared_ptr<amber::runtime::RuntimeTextWriter> stderr_buffer =
      amber::runtime::RuntimeTextWriter::buffer();
  amber::runtime::ExecutionResult parse_error;
  {
    amber::runtime::RuntimeOutputScope scope({}, stderr_buffer);
    parse_error = execute_source(
        "parser = ArgParser(cmdline: [\"--wat\"], name: \"tool\")\n"
        "parser.parse()\n");
  }
  expect(!parse_error.ok() && parse_error.fault.has_value() &&
             parse_error.fault->error_name == "ArgParser.UnknownOption",
         "ArgParser.parse parse errors terminate without becoming results");
  expect(stderr_buffer->to_string().find("usage: tool") != std::string::npos &&
             stderr_buffer->to_string().find(
                 "tool: error: unknown option --wat") != std::string::npos,
         "ArgParser.parse renders parse diagnostics to stderr");

  const std::shared_ptr<amber::runtime::RuntimeTextWriter> stdout_buffer =
      amber::runtime::RuntimeTextWriter::buffer();
  amber::runtime::ExecutionResult help;
  {
    amber::runtime::RuntimeOutputScope scope(stdout_buffer, {});
    help = execute_source(
        "parser = ArgParser(cmdline: [\"--help\"], name: \"tool\", "
        "about: \"A useful tool\")\n"
        "parser.parse()\n");
  }
  expect(help.ok() && help.value.is_null(),
         "ArgParser.parse handles help without an error fault");
  expect(stdout_buffer->to_string().find("usage: tool") != std::string::npos &&
             stdout_buffer->to_string().find("A useful tool") !=
                 std::string::npos,
         "ArgParser.parse renders help to stdout");
}

void test_declaration_validation() {
  expect_fault("ArgParser(cmdline: []).flag(\"verbose\")\n", "ArgumentError",
               "invalid option spelling");
  expect_fault("parser = ArgParser(cmdline: [])\n"
               "parser.arg(\"--items\", type: Array)\n",
               "ArgumentError", "unsupported converter type");
  expect_fault("parser = ArgParser(cmdline: [])\n"
               "parser.arg(\"--one\")\n"
               "parser.flag(\"--one\")\n",
               "ArgumentError", "duplicate option spelling");
}

} // namespace

int main() {
  test_options_positionals_and_rest();
  test_result_mode_choices_and_help();
  test_env_multiple_negatable_and_defaults();
  test_cmdline_override_and_local_converter();
  test_parse_or_raise_faults();
  test_first_class_errors_and_strict_equivalence();
  test_dotted_error_construction_and_matching();
  test_parse_error_rescue_hierarchy();
  test_converter_exception_boundaries();
  test_unhandled_error_name_and_trace();
  test_parse_cli_behavior();
  test_declaration_validation();

  std::cout << "stdlib_argparser_tests: ok\n";
  return 0;
}
