#include "bytecode/format.h"
#include "runtime/vm.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open input file: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string bytes = buffer.str();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
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

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2 && argc != 3) {
      std::cerr << "usage: amberbc_run <file.amberbc> [entry]\n";
      return 2;
    }

    const std::string path = argv[1];
    const std::string entry = argc == 3 ? argv[2] : "main";
    amber::bytecode::DecodeResult decoded =
        amber::bytecode::deserialize_module(read_file(path));
    if (!decoded.ok()) {
      for (const amber::bytecode::VerifyError &error : decoded.errors) {
        std::cerr << error.code << ": " << error.message;
        if (!error.section.empty()) {
          std::cerr << " in " << error.section;
        }
        if (error.offset != 0) {
          std::cerr << " at offset " << error.offset;
        }
        std::cerr << '\n';
      }
      return 1;
    }

    const amber::bytecode::BcMethod *method =
        method_by_name(decoded.module, entry);
    if (method == nullptr) {
      std::cerr << "missing entry method: " << entry << '\n';
      return 1;
    }

    amber::runtime::ExecutionResult result =
        amber::runtime::execute_code(decoded.module, method->entry_code_id);
    if (!result.ok()) {
      std::cerr << result.fault->error_name << ": " << result.fault->message
                << '\n';
      if (!result.fault->trace_text.empty()) {
        std::cerr << result.fault->trace_text << '\n';
      }
      return 1;
    }

    std::cout << amber::runtime::value_to_debug_string(result.value,
                                                       &decoded.module)
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "amberbc_run: " << error.what() << '\n';
    return 1;
  }
}
