# zpaqII — build del port (host Pascal + core C + asm)
#
#   make            core C + tests
#   make test       unit + diferencial contra upstream
#   make oracle     compila el upstream de referencia (binario oraculo)
#   make NOJIT=1    core sin JIT (solo interprete ZPAQL)
#
# El host Pascal se agrega en la etapa E5 (ver docs/PLAN.md).

CC       ?= gcc
CXX      ?= g++
AR       ?= ar
FPC      ?= fpc

CFLAGS   ?= -std=c11 -O3 -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
CPPFLAGS += -Iinclude
LDFLAGS  ?=

ifeq ($(NOJIT),1)
CPPFLAGS += -DZQ_NOJIT
endif

ifeq ($(DEBUG),1)
CFLAGS := -std=c11 -O0 -g -fsanitize=address,undefined -Wall -Wextra -Wshadow -Iinclude
LDFLAGS += -fsanitize=address,undefined
endif

UPSTREAM ?= /mnt/IA_LAB/agentes/zpaqII/upstream-zpaq
BUILD    := build

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

test: test-e1
	@echo "== E1 (hash/AES/scrypt): OK =="

test-e1: $(BUILD)/vectors_e1 $(BUILD)/e1_oracle
	@$(BUILD)/vectors_e1 > $(BUILD)/e1_core.txt
	@$(BUILD)/e1_oracle > $(BUILD)/e1_upstream.txt
	@diff -u $(BUILD)/e1_upstream.txt $(BUILD)/e1_core.txt && echo "E1: identico al oraculo ($$(wc -l < $(BUILD)/e1_core.txt) casos)"

$(BUILD)/vectors_e1: tests/unit/vectors_e1.c $(BUILD)/libzqcore.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libzqcore.a $(LDFLAGS) -o $@

$(BUILD)/e1_oracle: tools/oracle/e1_dump.cpp
	@mkdir -p $(BUILD)
	$(CXX) -O2 -Dunix -I$(UPSTREAM) $< $(UPSTREAM)/libzpaq.cpp -o $@

oracle:
	$(MAKE) -C $(UPSTREAM) zpaq

clean:
	rm -rf $(BUILD)
