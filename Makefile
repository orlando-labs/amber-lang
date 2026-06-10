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
BUILD_SRCS := buildsys/build.cpp
BYTECODE_SRCS := bytecode/format.cpp bytecode/emitter.cpp
IO_SRCS := runtime/io.cpp
RUNTIME_SRCS := runtime/context.cpp runtime/text.cpp $(IO_SRCS) runtime/vm.cpp runtime/module_loader.cpp runtime/native_bridge.cpp
FROZEN_RUNTIME_SRCS := runtime/frozen_image.cpp
PACKAGE_SRCS := package/package.cpp
FRONTEND_SRCS := $(LEXER_SRCS) $(AST_SRCS) $(PARSER_SRCS) $(PATTERN_SRCS) $(BINDER_SRCS) $(CHECKER_SRCS) $(HIR_SRCS)
CORE_SRCS := $(PROFILE_SRCS) $(BUILD_SRCS) $(FRONTEND_SRCS) $(MIR_SRCS) $(NATIVE_SRCS) $(BYTECODE_SRCS)
AMBERC_SRCS := tools/amberc/main.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS)
AMBERTEST_SRCS := tools/ambertest/main.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS)
IAMBER_SRCS := tools/iamber/main.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS)
IAMBER_LDLIBS ?= -lncurses
IAMBER_TEST_SRCS := tests/iamber_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS)
LEXER_TEST_SRCS := tests/lexer_tests.cpp $(LEXER_SRCS)
PARSER_TEST_SRCS := tests/parser_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
BINDER_TEST_SRCS := tests/binder_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
CHECKER_TEST_SRCS := tests/checker_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
WASM_ACCEL_TEST_SRCS := tests/wasm_accel_tests.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
MODERN_PROFILE_TEST_SRCS := tests/modern_profile_tests.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
BUILD_TEST_SRCS := tests/build_tests.cpp $(BUILD_SRCS)
HIR_TEST_SRCS := tests/hir_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
MIR_TEST_SRCS := tests/mir_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS) $(MIR_SRCS)
NATIVE_TEST_SRCS := tests/native_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
FROZEN_IMAGE_TEST_SRCS := tests/frozen_image_tests.cpp $(CORE_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS) $(RUNTIME_SRCS) $(FROZEN_RUNTIME_SRCS)
BYTECODE_TEST_SRCS := tests/bytecode_tests.cpp $(PROFILE_SRCS) $(BYTECODE_SRCS) $(LEXER_SRCS) $(AST_SRCS)
EMITTER_TEST_SRCS := tests/emitter_tests.cpp $(CORE_SRCS)
VM_TEST_SRCS := tests/vm_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_COLLECTIONS_TEST_SRCS := tests/stdlib_collections_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_TASK_TEST_SRCS := tests/stdlib_task_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
IO_TEST_SRCS := tests/io_tests.cpp runtime/context.cpp runtime/text.cpp $(IO_SRCS)
MODULE_LOADER_TEST_SRCS := tests/module_loader_tests.cpp $(PROFILE_SRCS) $(BUILD_SRCS) $(BYTECODE_SRCS) $(NATIVE_SRCS) $(LEXER_SRCS) $(AST_SRCS) $(RUNTIME_SRCS)
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
	buildsys/build.cpp \
	buildsys/build.h \
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
	runtime/context.cpp \
	runtime/context.h \
	runtime/io.cpp \
	runtime/io.h \
	runtime/text.cpp \
	runtime/frozen_image.cpp \
	runtime/frozen_image.h \
	runtime/vm.cpp \
	runtime/vm.h \
	package/package.cpp \
	package/package.h \
	tools/amberc/main.cpp \
	tools/iamber/main.cpp \
	tools/ambertest/main.cpp \
	tests/lexer_tests.cpp \
	tests/parser_tests.cpp \
	tests/binder_tests.cpp \
	tests/checker_tests.cpp \
	tests/wasm_accel_tests.cpp \
	tests/modern_profile_tests.cpp \
	tests/build_tests.cpp \
	tests/hir_tests.cpp \
	tests/mir_tests.cpp \
	tests/native_tests.cpp \
	tests/frozen_image_tests.cpp \
	tests/bytecode_tests.cpp \
	tests/emitter_tests.cpp \
	tests/module_loader_tests.cpp \
	tests/package_tests.cpp \
	tests/iamber_tests.cpp \
	tests/vm_tests.cpp \
	tests/stdlib_collections_tests.cpp \
	tests/stdlib_task_tests.cpp \
	tests/io_tests.cpp

.PHONY: all build test conformance spec-sync-check fmt clean

all: build

build: $(BUILD_DIR)/amberc $(BUILD_DIR)/ambertest $(BUILD_DIR)/iamber $(BUILD_DIR)/lexer_tests $(BUILD_DIR)/parser_tests $(BUILD_DIR)/binder_tests $(BUILD_DIR)/checker_tests $(BUILD_DIR)/wasm_accel_tests $(BUILD_DIR)/modern_profile_tests $(BUILD_DIR)/build_tests $(BUILD_DIR)/hir_tests $(BUILD_DIR)/mir_tests $(BUILD_DIR)/native_tests $(BUILD_DIR)/frozen_image_tests $(BUILD_DIR)/bytecode_tests $(BUILD_DIR)/emitter_tests $(BUILD_DIR)/vm_tests $(BUILD_DIR)/stdlib_collections_tests $(BUILD_DIR)/stdlib_task_tests $(BUILD_DIR)/io_tests $(BUILD_DIR)/module_loader_tests $(BUILD_DIR)/package_tests $(BUILD_DIR)/iamber_tests

$(BUILD_DIR)/.dir:
	mkdir -p $(BUILD_DIR)
	touch $(BUILD_DIR)/.dir

$(BUILD_DIR)/amberc: $(AMBERC_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(AMBERC_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/ambertest: $(AMBERTEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(AMBERTEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/iamber: $(IAMBER_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(IAMBER_SRCS) $(LDFLAGS) $(IAMBER_LDLIBS) -o $@

$(BUILD_DIR)/iamber_tests: $(IAMBER_TEST_SRCS) tools/iamber/main.cpp | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(IAMBER_TEST_SRCS) $(LDFLAGS) $(IAMBER_LDLIBS) -o $@

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

$(BUILD_DIR)/build_tests: $(BUILD_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BUILD_TEST_SRCS) $(LDFLAGS) -o $@

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

$(BUILD_DIR)/stdlib_collections_tests: $(STDLIB_COLLECTIONS_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_COLLECTIONS_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_task_tests: $(STDLIB_TASK_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_TASK_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/io_tests: $(IO_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(IO_TEST_SRCS) $(LDFLAGS) -o $@

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
	$(BUILD_DIR)/build_tests
	$(BUILD_DIR)/hir_tests
	$(BUILD_DIR)/mir_tests
	$(BUILD_DIR)/native_tests
	$(BUILD_DIR)/frozen_image_tests
	$(BUILD_DIR)/bytecode_tests
	$(BUILD_DIR)/emitter_tests
	$(BUILD_DIR)/vm_tests
	$(BUILD_DIR)/stdlib_collections_tests
	$(BUILD_DIR)/stdlib_task_tests
	$(BUILD_DIR)/io_tests
	$(BUILD_DIR)/module_loader_tests
	$(BUILD_DIR)/package_tests
	$(BUILD_DIR)/iamber_tests
	$(BUILD_DIR)/amberc lex corpus/parse/lexer/basic/source.am > $(BUILD_DIR)/lexer-basic.tokens.json
	$(BUILD_DIR)/amberc build tests/fixtures/w14_build/amber.build.json --out-dir $(BUILD_DIR)/w14_build/out --cache-dir $(BUILD_DIR)/w14_build/cache > $(BUILD_DIR)/w14-build-first.json
	$(BUILD_DIR)/amberc build tests/fixtures/w14_build/amber.build.json --target bytecode --out-dir $(BUILD_DIR)/w14_build/out --cache-dir $(BUILD_DIR)/w14_build/cache > $(BUILD_DIR)/w14-build-second.json
	$(BUILD_DIR)/amberc amberbc-verify $(BUILD_DIR)/w14_build/out/demo.main.amberbc > $(BUILD_DIR)/w14-main.verify.json
	$(BUILD_DIR)/amberc metadata $(BUILD_DIR)/w14_build/out/demo.main.amberbc --json > $(BUILD_DIR)/w14-main.metadata.json
	$(BUILD_DIR)/amberc verify $(BUILD_DIR)/w14_build/out/demo.main.amberbc --json > $(BUILD_DIR)/w14-main.public-verify.json
	! $(BUILD_DIR)/amberc verify tests/fixtures/bad.amberbc --json > $(BUILD_DIR)/bad.public-verify.json
	grep -q '"schema": "amber.bc.v1"' $(BUILD_DIR)/w14-main.metadata.json
	grep -q '"schema": "amber.bc.verify.v1"' $(BUILD_DIR)/w14-main.public-verify.json
	grep -q '"code":"BC1002"' $(BUILD_DIR)/bad.public-verify.json
	$(BUILD_DIR)/amberc amberbc-disasm $(BUILD_DIR)/w14_build/out/demo.main.amberbc > $(BUILD_DIR)/w14-main.disasm.txt
	grep -q '"native_output": "$(BUILD_DIR)/w14_build/out/demo.main"' $(BUILD_DIR)/w14-build-first.json
	$(BUILD_DIR)/w14_build/out/demo.main > $(BUILD_DIR)/w14-main-native-manifest.out
	grep -q '^42$$' $(BUILD_DIR)/w14-main-native-manifest.out
	$(BUILD_DIR)/amberc tests/fixtures/run_script/main.am > $(BUILD_DIR)/run-script.out
	grep -q '^42$$' $(BUILD_DIR)/run-script.out
	$(BUILD_DIR)/amberc build tests/fixtures/w14_build/src/main.am -o $(BUILD_DIR)/w14-main-exe > $(BUILD_DIR)/w14-main-exe-build.json
	grep -q '"entry": "main"' $(BUILD_DIR)/w14-main-exe-build.json
	grep -q '"native_entry": true' $(BUILD_DIR)/w14-main-exe-build.json
	$(BUILD_DIR)/w14-main-exe > $(BUILD_DIR)/w14-main-exe.out
	grep -q '^42$$' $(BUILD_DIR)/w14-main-exe.out
	$(BUILD_DIR)/amberc build bench/polyglot/amber/src/calls_collections.am --entry init -o $(BUILD_DIR)/calls-collections-native > $(BUILD_DIR)/calls-collections-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/calls-collections-native-build.json
	$(BUILD_DIR)/calls-collections-native > $(BUILD_DIR)/calls-collections-native.out
	grep -q '^2047795430$$' $(BUILD_DIR)/calls-collections-native.out
	$(BUILD_DIR)/ambertest run corpus

conformance: $(BUILD_DIR)/ambertest
	$(BUILD_DIR)/ambertest run corpus --bundle M11

spec-sync-check:
	python3 tools/spec_sync.py check

fmt:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(FORMAT_FILES); \
	else \
		echo "clang-format not found; skipping"; \
	fi

clean:
	rm -rf $(BUILD_DIR)
