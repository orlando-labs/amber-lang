BUILD_DIR ?= build

ifeq ($(origin CXX),default)
CXX := clang++
endif

CPPFLAGS ?=
CPPFLAGS += -I.

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS ?=

LEXER_SRCS := frontend/lexer/lexer.cpp frontend/lexer/token.cpp
AST_SRCS := frontend/ast/expr.cpp
PARSER_SRCS := frontend/parser/parser.cpp
PATTERN_SRCS := frontend/pattern/pattern.cpp
BINDER_SRCS := frontend/binder/binder.cpp
CHECKER_SRCS := frontend/checker/checker.cpp
HIR_SRCS := frontend/hir/hir.cpp
MIR_SRCS := optimizer/mir.cpp
NATIVE_SRCS := optimizer/native.cpp
FROZEN_SRCS := frozen/image.cpp
PROFILE_SRCS := profile/capabilities.cpp profile/effects.cpp profile/replay.cpp profile/data.cpp profile/wasm_accel.cpp profile/modern.cpp
BYTECODE_SRCS := bytecode/format.cpp bytecode/emitter.cpp
RUNTIME_SRCS := runtime/vm.cpp runtime/module_loader.cpp runtime/native_bridge.cpp
FROZEN_RUNTIME_SRCS := runtime/frozen_image.cpp
PACKAGE_SRCS := package/package.cpp
FRONTEND_SRCS := $(LEXER_SRCS) $(AST_SRCS) $(PARSER_SRCS) $(PATTERN_SRCS) $(BINDER_SRCS) $(CHECKER_SRCS) $(HIR_SRCS)
CORE_SRCS := $(PROFILE_SRCS) $(FRONTEND_SRCS) $(MIR_SRCS) $(NATIVE_SRCS) $(BYTECODE_SRCS)
AMBERC_SRCS := tools/amberc/main.cpp $(CORE_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS)
AMBERTEST_SRCS := tools/ambertest/main.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS)
LEXER_TEST_SRCS := tests/lexer_tests.cpp $(LEXER_SRCS)
PARSER_TEST_SRCS := tests/parser_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
BINDER_TEST_SRCS := tests/binder_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
CHECKER_TEST_SRCS := tests/checker_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
WASM_ACCEL_TEST_SRCS := tests/wasm_accel_tests.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
MODERN_PROFILE_TEST_SRCS := tests/modern_profile_tests.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
HIR_TEST_SRCS := tests/hir_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
MIR_TEST_SRCS := tests/mir_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS) $(MIR_SRCS)
NATIVE_TEST_SRCS := tests/native_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
FROZEN_IMAGE_TEST_SRCS := tests/frozen_image_tests.cpp $(CORE_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS) $(RUNTIME_SRCS) $(FROZEN_RUNTIME_SRCS)
BYTECODE_TEST_SRCS := tests/bytecode_tests.cpp $(PROFILE_SRCS) $(BYTECODE_SRCS) $(LEXER_SRCS) $(AST_SRCS)
EMITTER_TEST_SRCS := tests/emitter_tests.cpp $(CORE_SRCS)
VM_TEST_SRCS := tests/vm_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
MODULE_LOADER_TEST_SRCS := tests/module_loader_tests.cpp $(PROFILE_SRCS) $(BYTECODE_SRCS) $(NATIVE_SRCS) $(LEXER_SRCS) $(AST_SRCS) $(RUNTIME_SRCS)
PACKAGE_TEST_SRCS := tests/package_tests.cpp $(PROFILE_SRCS) $(PACKAGE_SRCS) $(LEXER_SRCS)

FORMAT_FILES := \
	frontend/lexer/lexer.cpp \
	frontend/lexer/lexer.h \
	frontend/lexer/token.cpp \
	frontend/lexer/token.h \
	frontend/ast/expr.cpp \
	frontend/ast/expr.h \
	frontend/parser/parser.cpp \
	frontend/parser/parser.h \
	frontend/pattern/pattern.cpp \
	frontend/pattern/pattern.h \
	frontend/binder/binder.cpp \
	frontend/binder/binder.h \
	frontend/checker/checker.cpp \
	frontend/checker/checker.h \
	frontend/hir/hir.cpp \
	frontend/hir/hir.h \
	optimizer/mir.cpp \
	optimizer/mir.h \
	optimizer/native.cpp \
	optimizer/native.h \
	profile/capabilities.cpp \
	profile/capabilities.h \
	profile/effects.cpp \
	profile/effects.h \
	profile/replay.cpp \
	profile/replay.h \
	profile/data.cpp \
	profile/data.h \
	profile/wasm_accel.cpp \
	profile/wasm_accel.h \
	profile/modern.cpp \
	profile/modern.h \
	frozen/image.cpp \
	frozen/image.h \
	bytecode/format.cpp \
	bytecode/format.h \
	bytecode/emitter.cpp \
	bytecode/emitter.h \
	runtime/module_loader.cpp \
	runtime/module_loader.h \
	runtime/native_bridge.cpp \
	runtime/native_bridge.h \
	runtime/frozen_image.cpp \
	runtime/frozen_image.h \
	runtime/vm.cpp \
	runtime/vm.h \
	package/package.cpp \
	package/package.h \
	tools/amberc/main.cpp \
	tools/ambertest/main.cpp \
	tests/lexer_tests.cpp \
	tests/parser_tests.cpp \
	tests/binder_tests.cpp \
	tests/checker_tests.cpp \
	tests/wasm_accel_tests.cpp \
	tests/modern_profile_tests.cpp \
	tests/hir_tests.cpp \
	tests/mir_tests.cpp \
	tests/native_tests.cpp \
	tests/frozen_image_tests.cpp \
	tests/bytecode_tests.cpp \
	tests/emitter_tests.cpp \
	tests/module_loader_tests.cpp \
	tests/package_tests.cpp \
	tests/vm_tests.cpp

.PHONY: all build test conformance fmt clean

all: build

build: $(BUILD_DIR)/amberc $(BUILD_DIR)/ambertest $(BUILD_DIR)/lexer_tests $(BUILD_DIR)/parser_tests $(BUILD_DIR)/binder_tests $(BUILD_DIR)/checker_tests $(BUILD_DIR)/wasm_accel_tests $(BUILD_DIR)/modern_profile_tests $(BUILD_DIR)/hir_tests $(BUILD_DIR)/mir_tests $(BUILD_DIR)/native_tests $(BUILD_DIR)/frozen_image_tests $(BUILD_DIR)/bytecode_tests $(BUILD_DIR)/emitter_tests $(BUILD_DIR)/vm_tests $(BUILD_DIR)/module_loader_tests $(BUILD_DIR)/package_tests

$(BUILD_DIR)/.dir:
	mkdir -p $(BUILD_DIR)
	touch $(BUILD_DIR)/.dir

$(BUILD_DIR)/amberc: $(AMBERC_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(AMBERC_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/ambertest: $(AMBERTEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(AMBERTEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/lexer_tests: $(LEXER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LEXER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/parser_tests: $(PARSER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PARSER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/binder_tests: $(BINDER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BINDER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/checker_tests: $(CHECKER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CHECKER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/wasm_accel_tests: $(WASM_ACCEL_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WASM_ACCEL_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/modern_profile_tests: $(MODERN_PROFILE_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODERN_PROFILE_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/hir_tests: $(HIR_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HIR_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/mir_tests: $(MIR_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MIR_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/native_tests: $(NATIVE_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/frozen_image_tests: $(FROZEN_IMAGE_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(FROZEN_IMAGE_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/bytecode_tests: $(BYTECODE_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BYTECODE_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/emitter_tests: $(EMITTER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(EMITTER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/vm_tests: $(VM_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(VM_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/module_loader_tests: $(MODULE_LOADER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_LOADER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/package_tests: $(PACKAGE_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PACKAGE_TEST_SRCS) $(LDFLAGS) -o $@

test: build
	$(BUILD_DIR)/lexer_tests
	$(BUILD_DIR)/parser_tests
	$(BUILD_DIR)/binder_tests
	$(BUILD_DIR)/checker_tests
	$(BUILD_DIR)/wasm_accel_tests
	$(BUILD_DIR)/modern_profile_tests
	$(BUILD_DIR)/hir_tests
	$(BUILD_DIR)/mir_tests
	$(BUILD_DIR)/native_tests
	$(BUILD_DIR)/frozen_image_tests
	$(BUILD_DIR)/bytecode_tests
	$(BUILD_DIR)/emitter_tests
	$(BUILD_DIR)/vm_tests
	$(BUILD_DIR)/module_loader_tests
	$(BUILD_DIR)/package_tests
	$(BUILD_DIR)/amberc lex corpus/parse/lexer/basic/source.am > $(BUILD_DIR)/lexer-basic.tokens.json
	$(BUILD_DIR)/ambertest run corpus

conformance: $(BUILD_DIR)/ambertest
	$(BUILD_DIR)/ambertest run corpus --bundle M5

fmt:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(FORMAT_FILES); \
	else \
		echo "clang-format not found; skipping"; \
	fi

clean:
	rm -rf $(BUILD_DIR)
