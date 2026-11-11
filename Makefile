# zpaqII - port build (Pascal host + C core + asm)
#
#   make            C core + tests
#   make test       unit + differential tests vs upstream
#   make oracle     clone/build the reference upstream (oracle binary)
#   make NOJIT=1    core without JIT (ZPAQL interpreter only)
#
# The Pascal host is added in stage E5 (see docs/PLAN.md).

CC       ?= gcc
CXX      ?= g++
AR       ?= ar
FPC      ?= fpc

CFLAGS   ?= -std=c11 -O3 -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
CPPFLAGS += -Iinclude -Isrc/core
LDFLAGS  ?=

ifeq ($(NOJIT),1)
CPPFLAGS += -DZQ_NOJIT
endif

ifeq ($(DEBUG),1)
CFLAGS := -std=c11 -O0 -g -fsanitize=address,undefined -Wall -Wextra -Wshadow -Iinclude -Isrc/core
LDFLAGS += -fsanitize=address,undefined
endif

UPSTREAM ?= include
BUILD    := build$(if $(DEBUG),-debug,)$(if $(NOJIT),-nojit,)

CORE_SRC := $(wildcard src/core/*.c)
CORE_OBJ := $(patsubst src/core/%.c,$(BUILD)/obj/%.o,$(CORE_SRC))

.PHONY: all lib test test-e1 test-sha1hw test-fragchain test-digest test-zpaqldif test-fastpath zdis cross win64 win32 oracle clean info

all: lib test

info:
	@echo "UPSTREAM=$(UPSTREAM)"
	@echo "CORE_SRC=$(CORE_SRC)"
	@echo "CFLAGS=$(CFLAGS)"

lib: $(BUILD)/libzqcore.a

$(BUILD)/libzqcore.a: $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: src/core/%.c include/zq.h src/core/zq_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---------------- tests ----------------

test: test-e1 test-e2 test-e3 test-e4 test-e4b test-e5 test-e6a test-e6c test-e18 test-e6d test-sha1hw test-fragchain test-digest test-zpaqldif test-fastpath
	@echo "== E1 (hash/AES/scrypt): OK =="
	@echo "== E2 (ZPAQL VM + compiler): OK =="
	@echo "== E3 (predictor + coder): OK =="
	@echo "== E4 (container + extraction): OK =="
	@echo "== E4b (bit-exact block compression): OK =="
	@echo "== E5 (Pascal host: streaming + journaling add/extract): OK =="
	@echo "== E6a (differential fuzzing): OK =="

test-e1: $(BUILD)/vectors_e1 $(BUILD)/e1_oracle
	@$(BUILD)/vectors_e1 > $(BUILD)/e1_core.txt
	@$(BUILD)/e1_oracle > $(BUILD)/e1_upstream.txt
	@diff -u $(BUILD)/e1_upstream.txt $(BUILD)/e1_core.txt && echo "E1: identical to the oracle ($$(wc -l < $(BUILD)/e1_core.txt) cases)"

$(BUILD)/vectors_e1: tests/unit/vectors_e1.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/e1_upstream.txt: $(BUILD)/e1_oracle
	@$(BUILD)/e1_oracle > $@

$(BUILD)/e1_oracle: tools/oracle/e1_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -DNOJIT -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

# Hardware SHA-1 (x86 SHA extensions) against the portable one.  On a CPU
# without SHA-NI this only reports that there is nothing to compare; run it
# under qemu -cpu max to exercise the hardware path (also part of `make test`).
test-fragchain: $(BUILD)/fragchain
	@$(BUILD)/fragchain

$(BUILD)/fragchain: tests/unit/fragchain.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

# ZPAQL(II): native fast paths for 7.15's own PCOMPs vs the reference
test-fastpath: $(BUILD)/fastpath_dif
	@$(BUILD)/fastpath_dif

$(BUILD)/fastpath_dif: tests/unit/fastpath_dif.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc/core $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

# ZPAQL(II): the new run loop against the one-step reference interpreter
test-zpaqldif: $(BUILD)/zpaql_dif $(BUILD)/e2_manifest.txt
	@$(BUILD)/zpaql_dif $(BUILD)/e2_manifest.txt

$(BUILD)/zpaql_dif: tests/unit/zpaql_dif.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc/core $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

test-digest: $(BUILD)/digest
	@$(BUILD)/digest

$(BUILD)/digest: tests/unit/digest.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

test-sha1hw: $(BUILD)/sha1_hw
	@$(BUILD)/sha1_hw
	@if [ "$$(uname -m)" = x86_64 ] && command -v qemu-x86_64 >/dev/null 2>&1; then \
	  echo "  sha1 hw under qemu -cpu max:"; \
	  qemu-x86_64 -cpu max $(BUILD)/sha1_hw; \
	fi

$(BUILD)/sha1_hw: tests/unit/sha1_hw.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

# Every target we can compile for, and every test we can run here.
cross: $(BUILD)/e1_upstream.txt
	@tools/cross.sh

# ZPAQL(II) tooling: disassembler and static verifier.
zdis: $(BUILD)/zdis
	@true

$(BUILD)/zdis: tools/zdis.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc/core $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

# Windows builds from Linux (mingw + FPC with the win64 units).
win64:
	@tools/win.sh win64

win32:
	@tools/win.sh win32

test-e2: $(BUILD)/zpaql_dump $(BUILD)/e2_oracle
	@$(BUILD)/e2_oracle $(BUILD)/e2_manifest.txt $(BUILD)/e2_upstream.txt
	@$(BUILD)/zpaql_dump $(BUILD)/e2_manifest.txt $(BUILD)/e2_core.txt
	@diff -u $(BUILD)/e2_upstream.txt $(BUILD)/e2_core.txt && echo "E2: identical to the oracle ($$(grep -c '^RESULT' $(BUILD)/e2_core.txt) configs)"

# The JIT only exists on x86-64 with a System V ABI (see zq_internal.h); the
# differential tests make no sense anywhere else.
HAVE_JIT := $(if $(NOJIT),,$(filter x86_64,$(shell uname -m)))

ifneq ($(HAVE_JIT),)
test-e6c: $(BUILD)/jit_diff
	@test -f $(BUILD)/e2_manifest.txt || $(BUILD)/e2_oracle $(BUILD)/e2_manifest.txt $(BUILD)/e2_upstream.txt
	@$(BUILD)/jit_diff $(BUILD)/e2_manifest.txt 600 64
	@echo "== E6c (ZPAQL JIT vs interpreter): OK =="
else
test-e6c:
	@echo "== E6c (ZPAQL JIT): skipped, no JIT on this target =="
endif

ifneq ($(HAVE_JIT),)
test-e18: $(BUILD)/pjit_diff
	@$(BUILD)/pjit_diff 300 512
	@echo "== E18 (predictor JIT vs interpreter): OK =="
else
test-e18:
	@echo "== E18 (predictor JIT): skipped, no JIT on this target =="
endif

$(BUILD)/pjit_diff: tests/unit/pjit_diff.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/jit_diff: tests/unit/jit_diff.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/zpaql_dump: tests/unit/zpaql_dump.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/e2_oracle: tools/oracle/e2_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -DNOJIT -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

test-e3: $(BUILD)/predictor_dump $(BUILD)/e3_oracle
	@$(BUILD)/e3_oracle $(BUILD)/e3_manifest.txt $(BUILD)/e3_upstream.txt
	@$(BUILD)/predictor_dump $(BUILD)/e3_manifest.txt $(BUILD)/e3_core.txt
	@diff -u $(BUILD)/e3_upstream.txt $(BUILD)/e3_core.txt && echo "E3: identical to the oracle ($$(grep -c '^RESULT' $(BUILD)/e3_core.txt) models, $$(grep -c '^P ' $(BUILD)/e3_core.txt) byte traces)"

$(BUILD)/predictor_dump: tests/unit/predictor_dump.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

# The E3 oracle is built with -DNOJIT so it measures the interpreter path that
# the port implements; JIT equivalence is tested separately.
$(BUILD)/e3_oracle: tools/oracle/e3_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -DNOJIT -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

oracle: $(UPSTREAM)/zpaq

$(UPSTREAM)/zpaq:
	@if [ ! -d $(UPSTREAM)/.git ]; then \
	  git clone --depth 1 https://github.com/zpaq/zpaq.git $(UPSTREAM); \
	fi
	{ $(MAKE) -C $(UPSTREAM) CPPFLAGS="-Dunix -DNOJIT" zpaq; }

clean:
	rm -rf build build-debug build-nojit build-debug-nojit

# E4 extraction matrix (upstream creates archives, the port extracts them)
$(BUILD)/extract: tests/unit/extract.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

test-e4: $(BUILD)/extract
	@tests/e4.sh $(BUILD)

# E4b: bit-exact block compression (port vs oracle)
$(BUILD)/cblock: tests/unit/cblock.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/e4_oracle: tools/oracle/e4_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -DNOJIT -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

test-e4b: $(BUILD)/cblock $(BUILD)/e4_oracle
	@tests/e4b.sh $(BUILD)

# E5: Pascal host
HOST_SRC := $(wildcard host/*.pas) host/zpaqii.lpr

$(BUILD)/faketime.so: tests/tools/faketime.c
	@mkdir -p $(BUILD)
	$(CC) -shared -fPIC -O2 $< -o $@

$(BUILD)/zpaqii: $(HOST_SRC) $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)/host
	$(FPC) -Mobjfpc -O2 -FU$(CURDIR)/$(BUILD)/host -Fu$(CURDIR)/host \
	  -Fl$(CURDIR)/$(BUILD) -k-lzqcore -k-lc host/zpaqii.lpr -o$@

host: $(BUILD)/zpaqii

matrix: $(BUILD)/zpaqii $(BUILD)/faketime.so
	@tests/matrix.sh $(BUILD)

bench: $(BUILD)/zpaqii $(BUILD)/faketime.so
	@tests/bench.sh $(BUILD)

test-e6d: $(BUILD)/zpaqii $(BUILD)/faketime.so
	@tests/cli.sh $(BUILD)
	@echo "== E6d (CLI options): OK =="

test-e6a: $(BUILD)/zpaqii
	@python3 tests/fuzz.py $(UPSTREAM)/zpaq $(BUILD)/zpaqii $(BUILD)/fuzzwork 60

ifeq ($(DEBUG),1)
# The ASan/UBSan core cannot be linked into the FPC binary (sanitizer runtime
# must come first); the host is exercised by the release build.
test-e5:
	@echo "test-e5: skipped in DEBUG (run plain 'make test-e5')"
test-e6d:
	@echo "test-e6d: skipped in DEBUG (run plain 'make test-e6d')"
else
test-e5: $(BUILD)/zpaqii $(BUILD)/faketime.so
	@tests/e5.sh $(BUILD)
endif
