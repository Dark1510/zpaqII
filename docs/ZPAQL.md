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
| JIT (x86-64) | `src/core/jit.c` | `docs/JIT.md`; phase B: W^X and batched runs |
| native fast paths | `src/core/fastpath.c` | the PCOMPs of 7.15's own methods, in C |
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
hand without it is guessing.

## Verification

* `make test-zpaqldif` (`tests/unit/zpaql_dif.c`) runs the one-step reference
  and the new loop side by side on the real configs of the E2 manifest plus
  ten method configs, and compares registers, flags, pc, H, M, R and the
  output stream after **every** input byte.
* `make test-e6c` (`jit_diff`) still compares the JIT against the reference on
  real and random programs.
* `make test`, `make matrix` and the fuzz suite exercise the new loop through
  real archives.

## Phase B (in progress)

### W^X

Both JITs (ZPAQL and predictor) map their code page `PROT_READ|PROT_WRITE`
while assembling and switch it to `PROT_READ|PROT_EXEC` with `mprotect`
before the first call (`zq_allocx_seal`).  Upstream keeps the page RWX for its
whole life.  Checked on a live extraction: no `rwx` mapping in
`/proc/<pid>/maps`, the JIT pages are `r-x`.

### Batched runs

`zq_zpaql_run_buf(z, buf, n)` is `zq_zpaql_run()` on `buf[0..n-1]`.  With the
JIT it is a single call into the generated code: `HALT` jumps to a small block
(`hnext` in `jit.c`) that loads the next byte into `a` and jumps back to the
first instruction, so the prologue and epilogue run once per batch instead of
once per byte.  The machine between two inputs is exactly what `HALT` would
have stored and the next call loaded.  Without the JIT it is the interpreter
loop over the buffer.

The decompresser decodes up to 4 KB, then hands the run to the postprocessor
(`zq_pp_write_buf`): one `run_buf` for a PCOMP, one copy into the output buffer
for a stored block.  If decoding fails part-way, the bytes decoded before the
failure still reach the postprocessor first and the same error is raised
again (`zq_d_fill`), so errors are reported exactly as before.  The
compressor's verification pass uses `run_buf` too.

### Native fast paths (`src/core/fastpath.c`)

The postprocessors 7.15's own methods generate are fixed programs:

| family | method | parameters | native |
|---|---|---|---|
| `lazy2` | level 1 LZ77 (variable-length codes) | E8E9, `rb` = args[0]−4 (0..8) | yes |
| `lzpre` | level 2 LZ77 (byte-aligned) | E8E9, `$3` = args[2] (0..255) | yes |
| `e8e9` | level 0 with E8E9 | — | yes |
| `bwtrle` | level 3 inverse BWT | E8E9, block ≤ 16 MB or not | recognised, still runs on the JIT |

When a block's PCOMP is loaded (`zq_zpaql_initp`), `zq_fast_detect()` reads the
parameters from the bytecode, compiles the template again with them and
compares the whole program byte for byte; only an exact match is run natively.
Archives do not change and plain zpaq 7.15 still runs the ZPAQL.

The native code is a line-by-line transcription of each program, quoted in the
comments: the same operations on the same 32-bit `a, b, c, d`, the same R
registers and the same masked M, so the machine after every input is the VM's.
The only liberty is in how a match is copied (`fp_copy`): in runs that wrap
neither index, with `memmove` when source and destination do not overlap
within the run and byte by byte when they do (an LZ77 repeat), and the output
written in bulk; the bytes are the same.  `ZPAQII_NOFASTPATH=1` disables it.

### Measured

Instructions executed (callgrind, deterministic) to extract 6 MB of text with
one thread, and CPU time (user+sys) to extract 35 MB of text on one pinned
core, median of 7 on a shared machine (so take it as ±10 %):

| archive | phase A | batched | batched + native | CPU time 35 MB |
|---|---|---|---|---|
| `-m1` | 530 M | 447 M | **352 M** (−34 %) | 741 → 533 ms (−28 %) |
| `-m2` | 507 M | 433 M | **334 M** (−34 %) | 385 → 272 ms (−29 %) |
| `-m3` | 11 705 M | 11 276 M | 11 282 M (−4 %) | 7470 → 7152 ms (−4 %) |

Under callgrind SHA-1 runs the portable code (no SHA-NI), so the absolute
counts overstate it; the differences are the engine.  `-m3` is dominated by
the context model, and its BWT postprocessor is not native yet.

### Verification

* `make test-zpaqldif` now also runs `zq_zpaql_run_buf` (JIT) against the
  one-step reference with batches of random length, on the HCOMPs and on the
  PCOMPs of 7.15's levels.  Like 7.15's JIT, ours does not store `f` when a
  comparison only feeds the `jt`/`jf` right after it, so `f` (dead there) and
  `pc` are not compared.
* `make test-fastpath` (`tests/unit/fastpath_dif.c`): every parameter variant
  is recognised and a one-byte change is rejected; native vs the one-step
  reference on random input (state after each batch, M and H at the end, then
  end of segment); and real blocks (text, and this executable for E8E9)
  compressed with 7.15's method strings, extracted with and without the fast
  path.  Six deliberate mutations of `fastpath.c` were each caught.
* `make test`, `make matrix` and the fuzz suite go through the fast path by
  default.

## Next

1. **Native inverse BWT** (`bwtrle`, both traversals): `-m3` text.
2. **Superinstructions** in the JIT for common pairs.
3. **Compile cache** keyed by the config, for archives with many blocks.
