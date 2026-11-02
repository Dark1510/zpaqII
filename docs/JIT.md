# ZPAQL JIT — design and status

Status: **scaffolding only**. `zq_zpaql_run()` already takes the JIT path when
`z->rcode != NULL`; `zq_zpaql_assemble()` currently returns 0, so every build
falls back to the interpreter and output is unchanged (`make test` green).

## Contract

Upstream compiles the block's HCOMP/PCOMP to x86-64 once (`ZPAQL::assemble`,
libzpaq.cpp 3033–3812) and calls it per input byte. The generated code returns:

| rc | meaning |
|---|---|
| 0 | OK (HALT reached) |
| 1 | bad ZPAQL opcode |
| 2 | out of memory |
| 3 | write error |
| other | ZPAQL execution error |

`zq_allocx()` allocates RWX memory with `mmap` and is already wired.

## Required state mapping

The generated code operates directly on `zq_zpaql` fields:

| ZPAQL | offset source |
|---|---|
| `a,b,c,d,f` | 32-bit registers in the struct |
| `pc`, `hbegin`, `hend` | control flow / bounds |
| `m` (`ZQ_M`), `h` (`ZQ_H`), `r` (`ZQ_R`) | base pointers, 64-byte aligned |
| `header[]` | program bytes, read for immediates and `LJ` |
| `outc()` | buffered output + flush |
| `run(input)` | `a = input; pc = hbegin; call code` |

## Implementation plan

1. **Emitter** (`src/core/jit.c`): a byte buffer with x86-64 encodings for the
   ~80 opcodes of `ZPAQL::execute`. Same opcode table as the interpreter; each
   case emits a short instruction sequence. Jumps are resolved with a two-pass
   label fixup (upstream uses the same approach).
2. **Entry stub**: save callee-saved registers, load the struct pointer, jump
   to the first instruction; the HALT path returns 0.
3. **Fallback**: any unsupported opcode → `assemble()` returns 0 and the
   interpreter runs (already the behavior today).
4. **Differential test**: `tests/unit/zpaql_dump.c` already compares bytecode
   and execution traces against upstream's interpreter; add a variant that
   compiles with the JIT and compares H[]/output for 10^6 random steps, plus
   `make NOJIT=1` producing identical archive bytes.

## Order of work

1. `pc` handling and jump opcodes (`JT/JF/JMP/LJ`), `a=r`/`r=a`, `halt`.
2. Arithmetic/logic/compare opcodes.
3. Memory (`*b/*c/*d`) and `hash`/`hashd`/`out`.
4. Error paths and the `LJ` bounds check.
5. `Predictor::assemble_p` (the predictor loop JIT) is a separate, later step.

Each step is verified before the next: the matrix and the fuzz test must stay
green with the JIT enabled and disabled.
