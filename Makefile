BUILD_DIR ?= build

ifeq ($(origin CXX),default)
CXX := clang++
endif

CPPFLAGS ?=
CPPFLAGS += -I.

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS ?=

# --- Allocator selection -----------------------------------------------------
# MALLOC selects the C/C++ allocator the binaries link against (RESEARCH heap
# fragmentation, §10 step 1). Flavors:
#   system   (default) -- platform malloc, no extra dependency
#   mimalloc            -- recommended; best macOS story, aggressive page return
#   jemalloc           -- Linux deploys + diagnosis (stats_print, heap profiling)
# Install via `brew install mimalloc` / `brew install jemalloc`, or point
# MIMALLOC_PREFIX / JEMALLOC_PREFIX at an existing install.
#
# Page return is a *runtime* knob, not a build flag -- set it when you run:
#   mimalloc: MIMALLOC_PURGE_DELAY=0            (decommit dirty pages immediately)
#   jemalloc: MALLOC_CONF=background_thread:true,dirty_decay_ms:0,muzzy_decay_ms:0
# Use AMBER_HEAP_STATS=1 on a run to print the §9 RSS / live-bytes line.
MALLOC ?= system
UNAME_S := $(shell uname -s)

ifeq ($(MALLOC),system)
CPPFLAGS += -DAMBER_ALLOCATOR=\"system\"
else ifeq ($(MALLOC),mimalloc)
CPPFLAGS += -DAMBER_ALLOCATOR=\"mimalloc\"
MIMALLOC_PREFIX ?= $(shell brew --prefix mimalloc 2>/dev/null)
ifeq ($(strip $(MIMALLOC_PREFIX)),)
$(error MALLOC=mimalloc but mimalloc was not found; run `brew install mimalloc` or set MIMALLOC_PREFIX)
endif
CPPFLAGS += -I$(MIMALLOC_PREFIX)/include
MIMALLOC_STATIC := $(firstword $(wildcard $(MIMALLOC_PREFIX)/lib/libmimalloc.a))
ifeq ($(UNAME_S),Darwin)
# Force-load the static archive so mimalloc's malloc-zone override wins over the
# system allocator; fall back to dynamic linking if no static archive is present.
ifeq ($(strip $(MIMALLOC_STATIC)),)
LDFLAGS += -L$(MIMALLOC_PREFIX)/lib -lmimalloc
else
LDFLAGS += -Wl,-force_load,$(MIMALLOC_STATIC)
endif
else
LDFLAGS += -L$(MIMALLOC_PREFIX)/lib -Wl,-rpath,$(MIMALLOC_PREFIX)/lib -lmimalloc
endif
else ifeq ($(MALLOC),jemalloc)
CPPFLAGS += -DAMBER_ALLOCATOR=\"jemalloc\"
JEMALLOC_PREFIX ?= $(shell brew --prefix jemalloc 2>/dev/null)
ifeq ($(strip $(JEMALLOC_PREFIX)),)
$(error MALLOC=jemalloc but jemalloc was not found; run `brew install jemalloc` or set JEMALLOC_PREFIX)
endif
CPPFLAGS += -I$(JEMALLOC_PREFIX)/include
JEMALLOC_STATIC := $(firstword $(wildcard $(JEMALLOC_PREFIX)/lib/libjemalloc.a))
ifeq ($(UNAME_S),Darwin)
# jemalloc registers its malloc zone from a static-archive constructor; force-load
# it so the override takes effect (macOS jemalloc is second-tier -- see RESEARCH §4).
ifeq ($(strip $(JEMALLOC_STATIC)),)
LDFLAGS += -L$(JEMALLOC_PREFIX)/lib -ljemalloc
else
LDFLAGS += -Wl,-force_load,$(JEMALLOC_STATIC)
endif
else
LDFLAGS += -L$(JEMALLOC_PREFIX)/lib -Wl,-rpath,$(JEMALLOC_PREFIX)/lib -ljemalloc
endif
else
$(error Unknown MALLOC='$(MALLOC)'; use system, mimalloc, or jemalloc)
endif
# -----------------------------------------------------------------------------

# --- Value representation selection ------------------------------------------
# VALUE_REPR selects the runtime `Value` storage (PLAN Phase 4 value-repr
# prototype). Flavors:
#   variant  (default) -- 24-byte std::variant Value, today's behaviour.
#   tagged             -- 16-byte tagged-union Value (prototype). Immediates and
#                         the six ObjHeader heap kinds are stored inline; the
#                         ~15 cold tail types (BigInt/error/task/io/...) are
#                         boxed behind a refcounted TailBox (+1 alloc per tail
#                         value, all cold paths). Defines AMBER_VALUE_REPR_TAGGED.
# A/B a tagged interpreter against the default with:
#   make build/iamber VALUE_REPR=tagged
# (do not mix object files across reps -- rebuild from clean or use a fresh
# BUILD_DIR, since the flag changes sizeof(Value) ABI-wide).
VALUE_REPR ?= variant
ifeq ($(VALUE_REPR),variant)
# default storage; no macro needed
else ifeq ($(VALUE_REPR),tagged)
CPPFLAGS += -DAMBER_VALUE_REPR_TAGGED
else
$(error Unknown VALUE_REPR='$(VALUE_REPR)'; use variant or tagged)
endif
# -----------------------------------------------------------------------------

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
DIGEST_SRCS := runtime/digest.cpp
STDLIB_SRCS := runtime/stdlib_registry.cpp runtime/stdlib_math.cpp runtime/stdlib_json.cpp runtime/stdlib_codecs.cpp runtime/stdlib_digest.cpp runtime/stdlib_secure_random.cpp runtime/stdlib_argparser.cpp runtime/stdlib_uuid.cpp runtime/stdlib_time.cpp runtime/stdlib_url.cpp
RUNTIME_SRCS := runtime/context.cpp runtime/text.cpp $(IO_SRCS) $(DIGEST_SRCS) runtime/vm.cpp $(STDLIB_SRCS) runtime/amber_ext.cpp runtime/module_loader.cpp runtime/native_bridge.cpp
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
STDLIB_REGISTRY_TEST_SRCS := tests/stdlib_registry_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_JSON_TEST_SRCS := tests/stdlib_json_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_CODECS_TEST_SRCS := tests/stdlib_codecs_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_DIGEST_TEST_SRCS := tests/stdlib_digest_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_SECURE_RANDOM_TEST_SRCS := tests/stdlib_secure_random_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_ARGPARSER_TEST_SRCS := tests/stdlib_argparser_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_UUID_TEST_SRCS := tests/stdlib_uuid_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_TIME_TEST_SRCS := tests/stdlib_time_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_URL_TEST_SRCS := tests/stdlib_url_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
AMBER_EXT_TEST_SRCS := tests/amber_ext_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
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
	runtime/digest.cpp \
	runtime/digest.h \
	runtime/text.cpp \
	runtime/frozen_image.cpp \
	runtime/frozen_image.h \
	runtime/vm.cpp \
	runtime/vm.h \
	runtime/stdlib_registry.cpp \
	runtime/stdlib_registry.h \
	runtime/stdlib_math.cpp \
	runtime/stdlib_json.cpp \
	runtime/stdlib_codecs.cpp \
	runtime/stdlib_digest.cpp \
	runtime/stdlib_secure_random.cpp \
	runtime/stdlib_argparser.cpp \
	runtime/stdlib_uuid.h \
	runtime/stdlib_uuid.cpp \
	runtime/stdlib_time.cpp \
	runtime/stdlib_url.h \
	runtime/stdlib_url.cpp \
	runtime/amber_ext.h \
	runtime/amber_ext_runtime.h \
	runtime/amber_ext.cpp \
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
	tests/stdlib_registry_tests.cpp \
	tests/stdlib_json_tests.cpp \
	tests/stdlib_codecs_tests.cpp \
	tests/stdlib_digest_tests.cpp \
	tests/stdlib_secure_random_tests.cpp \
	tests/stdlib_argparser_tests.cpp \
	tests/stdlib_uuid_tests.cpp \
	tests/stdlib_time_tests.cpp \
	tests/io_tests.cpp

.PHONY: all build test conformance backend-equivalence spec-sync-check fmt clean

all: build

build: $(BUILD_DIR)/amberc $(BUILD_DIR)/ambertest $(BUILD_DIR)/iamber $(BUILD_DIR)/lexer_tests $(BUILD_DIR)/parser_tests $(BUILD_DIR)/binder_tests $(BUILD_DIR)/checker_tests $(BUILD_DIR)/wasm_accel_tests $(BUILD_DIR)/modern_profile_tests $(BUILD_DIR)/build_tests $(BUILD_DIR)/hir_tests $(BUILD_DIR)/mir_tests $(BUILD_DIR)/native_tests $(BUILD_DIR)/frozen_image_tests $(BUILD_DIR)/bytecode_tests $(BUILD_DIR)/emitter_tests $(BUILD_DIR)/vm_tests $(BUILD_DIR)/stdlib_collections_tests $(BUILD_DIR)/stdlib_task_tests $(BUILD_DIR)/stdlib_registry_tests $(BUILD_DIR)/stdlib_json_tests $(BUILD_DIR)/stdlib_codecs_tests $(BUILD_DIR)/stdlib_digest_tests $(BUILD_DIR)/stdlib_secure_random_tests $(BUILD_DIR)/stdlib_argparser_tests $(BUILD_DIR)/stdlib_uuid_tests $(BUILD_DIR)/stdlib_time_tests $(BUILD_DIR)/stdlib_url_tests $(BUILD_DIR)/amber_ext_tests $(BUILD_DIR)/io_tests $(BUILD_DIR)/module_loader_tests $(BUILD_DIR)/package_tests $(BUILD_DIR)/iamber_tests

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

$(BUILD_DIR)/stdlib_registry_tests: $(STDLIB_REGISTRY_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_REGISTRY_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_json_tests: $(STDLIB_JSON_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_JSON_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_codecs_tests: $(STDLIB_CODECS_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_CODECS_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_digest_tests: $(STDLIB_DIGEST_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_DIGEST_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_secure_random_tests: $(STDLIB_SECURE_RANDOM_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_SECURE_RANDOM_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_argparser_tests: $(STDLIB_ARGPARSER_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_ARGPARSER_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_uuid_tests: $(STDLIB_UUID_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_UUID_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_time_tests: $(STDLIB_TIME_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_TIME_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/stdlib_url_tests: $(STDLIB_URL_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(STDLIB_URL_TEST_SRCS) $(LDFLAGS) -o $@

$(BUILD_DIR)/amber_ext_tests: $(AMBER_EXT_TEST_SRCS) | $(BUILD_DIR)/.dir
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(AMBER_EXT_TEST_SRCS) $(LDFLAGS) -o $@

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
	$(BUILD_DIR)/stdlib_registry_tests
	$(BUILD_DIR)/stdlib_json_tests
	$(BUILD_DIR)/stdlib_codecs_tests
	$(BUILD_DIR)/stdlib_digest_tests
	$(BUILD_DIR)/stdlib_secure_random_tests
	$(BUILD_DIR)/stdlib_argparser_tests
	$(BUILD_DIR)/stdlib_uuid_tests
	$(BUILD_DIR)/stdlib_time_tests
	$(BUILD_DIR)/stdlib_url_tests
	$(BUILD_DIR)/amber_ext_tests
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
	$(BUILD_DIR)/amberc build bench/polyglot/amber/src/sha_digest.am --entry init -o $(BUILD_DIR)/sha-digest-native > $(BUILD_DIR)/sha-digest-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/sha-digest-native-build.json
	$(BUILD_DIR)/sha-digest-native > $(BUILD_DIR)/sha-digest-native.out
	grep -q '^5616000$$' $(BUILD_DIR)/sha-digest-native.out
	$(BUILD_DIR)/amberc build tests/fixtures/secure_random_native/main.am --entry main-only --grant random.secure -o $(BUILD_DIR)/secure-random-native > $(BUILD_DIR)/secure-random-native-build.json
	grep -q '"native_entry": true' $(BUILD_DIR)/secure-random-native-build.json
	$(BUILD_DIR)/secure-random-native > $(BUILD_DIR)/secure-random-native.out
	grep -q '^42$$' $(BUILD_DIR)/secure-random-native.out
	$(BUILD_DIR)/amberc build tests/fixtures/secure_random_native/int.am --entry main-only --grant random.secure -o $(BUILD_DIR)/secure-random-native-int > $(BUILD_DIR)/secure-random-native-int-build.json
	$(BUILD_DIR)/secure-random-native-int > $(BUILD_DIR)/secure-random-native-int.out
	grep -q '^42$$' $(BUILD_DIR)/secure-random-native-int.out
	$(BUILD_DIR)/amberc build tests/fixtures/uuid_native/main.am --entry main-only --grant random.secure -o $(BUILD_DIR)/uuid-native > $(BUILD_DIR)/uuid-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/uuid-native-build.json
	$(BUILD_DIR)/uuid-native > $(BUILD_DIR)/uuid-native.out
	grep -q '^42$$' $(BUILD_DIR)/uuid-native.out
	$(BUILD_DIR)/amberc build tests/fixtures/digest_native/main.am --entry main-only -o $(BUILD_DIR)/digest-native > $(BUILD_DIR)/digest-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/digest-native-build.json
	$(BUILD_DIR)/digest-native > $(BUILD_DIR)/digest-native.out
	grep -q '^42$$' $(BUILD_DIR)/digest-native.out
	$(BUILD_DIR)/amberc build tests/fixtures/url_native/source.am -o $(BUILD_DIR)/url-native > $(BUILD_DIR)/url-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/url-native-build.json
	$(BUILD_DIR)/url-native > $(BUILD_DIR)/url-native.out
	grep -q '^42$$' $(BUILD_DIR)/url-native.out
	$(BUILD_DIR)/amberc build bench/polyglot/amber/src/time_flow.am --entry main-only -o $(BUILD_DIR)/time-flow-native > $(BUILD_DIR)/time-flow-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/time-flow-native-build.json
	$(BUILD_DIR)/time-flow-native > $(BUILD_DIR)/time-flow-native.out
	grep -q '^110397732$$' $(BUILD_DIR)/time-flow-native.out
	rm -rf $(BUILD_DIR)/native_ext_demo
	$(BUILD_DIR)/amberc build tests/fixtures/native_ext_demo/amber.build.json --target native --out-dir $(BUILD_DIR)/native_ext_demo/out --cache-dir $(BUILD_DIR)/native_ext_demo/cache > $(BUILD_DIR)/native-ext-demo-build.json
	grep -q '"status": "ok"' $(BUILD_DIR)/native-ext-demo-build.json
	$(BUILD_DIR)/native_ext_demo/out/nat.demo > $(BUILD_DIR)/native-ext-demo-native.out
	grep -q '^42$$' $(BUILD_DIR)/native-ext-demo-native.out
	$(BUILD_DIR)/amberc tests/fixtures/native_ext_demo/src/main.am > $(BUILD_DIR)/native-ext-demo-bytecode.out
	grep -q '^210$$' $(BUILD_DIR)/native-ext-demo-bytecode.out
	rm -rf $(BUILD_DIR)/native_class_demo
	$(BUILD_DIR)/amberc build tests/fixtures/native_class_demo/amber.build.json --target native --out-dir $(BUILD_DIR)/native_class_demo/out --cache-dir $(BUILD_DIR)/native_class_demo/cache > $(BUILD_DIR)/native-class-demo-build.json
	grep -q '"status": "ok"' $(BUILD_DIR)/native-class-demo-build.json
	$(BUILD_DIR)/native_class_demo/out/nat.box > $(BUILD_DIR)/native-class-demo-native.out
	grep -q '^23$$' $(BUILD_DIR)/native-class-demo-native.out
	! $(BUILD_DIR)/amberc tests/fixtures/native_class_demo/src/main.am > $(BUILD_DIR)/native-class-demo-bytecode.out 2>&1
	$(BUILD_DIR)/ambertest run corpus

conformance: $(BUILD_DIR)/ambertest
	$(BUILD_DIR)/ambertest run corpus --bundle M11

backend-equivalence: $(BUILD_DIR)/amberc
	python3 tools/backend_equivalence.py

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
