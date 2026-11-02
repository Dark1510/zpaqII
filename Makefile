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

UPSTREAM ?= /mnt/IA_LAB/agentes/zpaqII/upstream-zpaq
BUILD    := build$(if $(DEBUG),-debug,)$(if $(NOJIT),-nojit,)

CORE_SRC := $(wildcard src/core/*.c)
CORE_OBJ := $(patsubst src/core/%.c,$(BUILD)/obj/%.o,$(CORE_SRC))

.PHONY: all lib test test-e1 oracle clean info

all: lib test

info:
	@echo "UPSTREAM=$(UPSTREAM)"
	@echo "CORE_SRC=$(CORE_SRC)"
	@echo "CFLAGS=$(CFLAGS)"

lib: $(BUILD)/libzqcore.a

$(BUILD)/libzqcore.a: $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: src/core/%.c include/zq.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---------------- tests ----------------

test: test-e1 test-e2 test-e3 test-e4 test-e4b test-e5
	@echo "== E1 (hash/AES/scrypt): OK =="
	@echo "== E2 (ZPAQL VM + compiler): OK =="
	@echo "== E3 (predictor + coder): OK =="
	@echo "== E4 (container + extraction): OK =="
	@echo "== E4b (bit-exact block compression): OK =="
	@echo "== E5 (Pascal host: streaming + journaling add/extract): OK =="

test-e1: $(BUILD)/vectors_e1 $(BUILD)/e1_oracle
	@$(BUILD)/vectors_e1 > $(BUILD)/e1_core.txt
	@$(BUILD)/e1_oracle > $(BUILD)/e1_upstream.txt
	@diff -u $(BUILD)/e1_upstream.txt $(BUILD)/e1_core.txt && echo "E1: identical to the oracle ($$(wc -l < $(BUILD)/e1_core.txt) cases)"

$(BUILD)/vectors_e1: tests/unit/vectors_e1.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/e1_oracle: tools/oracle/e1_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

test-e2: $(BUILD)/zpaql_dump $(BUILD)/e2_oracle
	@$(BUILD)/e2_oracle $(BUILD)/e2_manifest.txt $(BUILD)/e2_upstream.txt
	@$(BUILD)/zpaql_dump $(BUILD)/e2_manifest.txt $(BUILD)/e2_core.txt
	@diff -u $(BUILD)/e2_upstream.txt $(BUILD)/e2_core.txt && echo "E2: identical to the oracle ($$(grep -c '^RESULT' $(BUILD)/e2_core.txt) configs)"

$(BUILD)/zpaql_dump: tests/unit/zpaql_dump.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/e2_oracle: tools/oracle/e2_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

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
	$(MAKE) -C $(UPSTREAM) zpaq

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
	$(CXX) -O2 -Dunix -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

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

ifeq ($(DEBUG),1)
# The ASan/UBSan core cannot be linked into the FPC binary (sanitizer runtime
# must come first); the host is exercised by the release build.
test-e5:
	@echo "test-e5: skipped in DEBUG (run plain 'make test-e5')"
else
test-e5: $(BUILD)/zpaqii $(BUILD)/faketime.so
	@tests/e5.sh $(BUILD)
endif
