# ZPAQL(II)

ZPAQL(II) is the ZPAQL engine of zpaqII: **the same instruction set, the same
semantics, a new engine**.  The name is documentation only — the bytecode, the
archives and the behaviour are zpaq 7.15's.

## The contract

1. **Same bytecode.** Any program that 7.15 accepts runs here with the same
   results, including the quirks we reproduce on purpose: a run of identical
   `X<>A` merged by `oplen()`, `swap(U8&)` keeping the high 24 bits of `a`, an
   error when `pc` leaves the program.
2. **Archives we write still run on 7.15.** No opcode was added or changed.
3. **Observable state is the struct.** Registers, flags, memory and output are
   written back when a run ends, so callers and tests see the same machine.

## The engine

| piece | where | notes |
|---|---|---|
| interpreter | `zq_zpaql_run_interp()` in `src/core/zpaql.c` | one loop, state in locals, one dispatch per instruction |
| one-step reference | `zq_zpaql_execute()` | the old per-instruction stepper, kept as the reference the differential test compares against |
| JIT (x86-64) | `src/core/jit.c` | unchanged, `docs/JIT.md` |
| tooling | `tools/zdis.c` | disassembler and static verifier |

The interpreter keeps `a, b, c, d, f, pc` in C locals for the whole input byte
and only writes them back at `HALT` (or when the run ends through an error).
Before, every instruction went through the struct: one dispatch plus six
memory accesses per instruction.  Everything else — the opcode semantics, the
masked M/H addressing, the output buffer and its flush — is identical.

## Measured

Extraction of a 30 MB text corpus, `taskset -c 7`, minimum of 3, interpreter
build (`NOJIT=1`) versus the previous interpreter:

| archive | interpreter before | interpreter now | speed-up | JIT reference |
|---|---|---|---|---|
| `-m1` | 2.49 s | **1.12 s** | 2.2× | 0.32 s |
| `-m2` | 2.02 s | **0.88 s** | 2.3× | 0.29 s |
| `-m4` | 14.20 s | **11.76 s** | 1.2× | 8.71 s |
| `-m5` | 116.98 s | **104.66 s** | 1.1× | 66.94 s |

`-m1`/`-m2` are the ZPAQL-heavy methods (LZ77 and E8E9 live in the PCOMP), so
they show the engine; `-m4`/`-m5` are dominated by the predictor, which the
`NOJIT` build also runs in C.

This is what ARM64, Windows and `NOJIT` builds use, so those platforms gained
the most: on x86-64 the JIT already covered it.

## Tooling: `zdis`

```
make zdis
./build/zdis -m x4        # the config of a method, disassembled
./build/zdis file.conf    # any config text
```

It prints the HCOMP and PCOMP programs with their offsets and checks what can
be checked statically: jumps inside the program, operand bytes that are
actually there, invalid opcodes.  It exists because writing a ZPAQL decoder by
hand without it is guessing (see phase 2.4).

## Verification

* `make test-zpaqldif` (`tests/unit/zpaql_dif.c`) runs the one-step reference
  and the new loop side by side on the real configs of the E2 manifest plus
  ten method configs, and compares registers, flags, pc, H, M, R and the
  output stream after **every** input byte.
* `make test-e6c` (`jit_diff`) still compares the JIT against the reference on
  real and random programs.
* `make test`, `make matrix` and the fuzz suite exercise the new loop through
  real archives.

## Next

In rough order of value:

1. **Native fast paths for recognised PCOMPs** (with 2.4): fingerprint the
   bytecode, decode in C, skip the VM.  Compatible: 7.15 still runs the ZPAQL.
2. **JIT batching**: enter the generated code once per N bytes instead of once
   per byte (the prologue/epilogue currently costs more than short programs).
3. **Superinstructions** in the JIT for common pairs.
4. **Compile cache** keyed by the config, for archives with many blocks.
5. **W^X**: the generated code page is currently `PROT_READ|WRITE|EXEC`.
