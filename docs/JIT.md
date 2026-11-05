# JIT — design and status

Two JITs, both direct ports of libzpaq 7.15:

| function | upstream | port | runs from |
|---|---|---|---|
| HCOMP/PCOMP (ZPAQL program) | `ZPAQL::assemble()` | `zq_zpaql_assemble()` (`jit.c`) | `zq_zpaql_run()` |
| predictor components | `Predictor::assemble_p()` | `zq_pred_assemble()` (`pjit.c`) | `zq_pred_predict/update()` |

Both are x86-64 only, both fall back to the interpreter when no code could be
produced, and `make NOJIT=1` removes them entirely.

## Contract

Upstream compiles the block's HCOMP/PCOMP to x86-64 once and calls it per input
byte. The generated code returns:

| rc | meaning |
|---|---|
| 0 | OK (HALT reached) |
| 1 | bad ZPAQL opcode or jump out of bounds |
| 2 | out of memory |
| 3 | write error |

`zq_zpaql_run()` maps those to the same messages as upstream (`Bad ZPAQL
opcode`, `Out of memory`, `Write error`, `ZPAQL execution error`).

## State mapping

The generated code keeps the machine state in registers, exactly like upstream:

| ZPAQL | x86-64 |
|---|---|
| a | `edx` |
| b | `esi` |
| c | `edi` |
| d | `ebp` |
| f | `ebx` |
| h | `r12` |
| outbuf | `r13` |
| r | `r14` |
| m | `r15` |

`a`, `b`, `c`, `d` and `f` are read from the `zq_zpaql` struct in the prologue and
written back in the HALT block, so the struct is the only observable state.

## Structure of the generated code

1. `jmp start` at offset 0.
2. HALT block: store the registers back, restore the callee-saved registers, `ret`.
   The `HALT` instruction does not go there directly but to `hnext`
   (ZPAQL(II) batching): while `z->bn > 0` it loads the next byte of the batch
   (`z->bin`) into `a` and jumps back to the first instruction; otherwise it
   falls into the HALT block.  Errors still go straight to the HALT block.
3. OUT block (a local subroutine): `outbuf[bufptr++] = a`, and when the buffer
   fills, call `zq_jit_flush()` with the registers saved.
4. `start`: save registers, load the state, then the translated program.
5. A second pass patches every jump target.

Code lives in an `mmap`ed page (`zq_allocx`), RW while it is assembled and
RX once `zq_allocx_seal` has run (W^X; upstream keeps it RWX); it is freed whenever the
header or the h/m arrays change, because the generated code bakes in the
addresses of `a…f`, `bufptr`, `h`, `m`, `r` and `outbuf`.

## Verification

* `tests/unit/jit_diff.c` (`make test-e6c`) runs the JIT and the interpreter
  side by side over
  - every config of the E2 manifest (26 real HCOMP/PCOMP programs), and
  - randomly generated, syntactically valid HCOMP programs (600 by default,
    0 failures with 3000 while developing),
  and compares registers, H, M and the output stream after every input. Error
  paths are compared too (both must raise).
* The bit-exact suites run with the JIT on by default: `make test`, `make
  matrix` (102 checks) and `tests/fuzz.py`. `make NOJIT=1` builds the
  interpreter-only core and passes E1–E4 as well.

## Known upstream quirks reproduced on purpose

**Runs of identical swap bytes.** `oplen()` merges a run of identical instruction
bytes into one instruction. That is exact for `X++`/`X--` (the emitted `add`
carries the count) but not for `X<>A`: `c<>a c<>a` is assembled as a single
`xchg`, so the JIT performs one swap where the interpreter performs two.
Upstream has exactly the same behavior (verified against the reference binary:
`a=0xFFFFFF00, c<>a, c<>a` gives `a=0xFFFFFF00` in `run0()` and `a=0` in
`run()`); `zpaq`'s own configs never contain such a run, and the port reproduces
it rather than diverging. `jit_diff` checks this case explicitly.

**Swap with \*b or \*c keeps the high bits of a.** `a<>*b` exchanges only the low
byte of `a`; upstream's `swap(U8&)` XORs with a zero-extended byte, so the top 24
bits survive. This is documented in the ZPAQL opcode table and implemented that
way in the interpreter (it was the one real bug the differential test found in
the C core).

**Code is invalidated by `init()`.** Upstream frees `rcode` only in `read()` and
`clear()`; `init()` reallocates `h`/`m` with `Array::resize`, which frees the old
blocks, so previously assembled code points at freed memory. Upstream survives
only because `calloc` tends to return the same address; the port frees `rcode`
in `zq_zpaql_init_sizes()` so a fresh block always re-assembles.

# Predictor JIT

`zq_pred_assemble()` is a direct port of `Predictor::assemble_p()`
(libzpaq.cpp 3824–4590). One buffer holds both entry points:

| offset | code |
|---|---|
| `pcode[0]` | `jmp predict` |
| `pcode[5]` | `jmp update` |
| `pcode[10]` | `predict(zq_predictor*) -> eax` |
| `pcode[15]` | `update(zq_predictor*, int y)` |

The predictor address arrives in `rdi` and the bit `y` in `rsi` (Linux-64 ABI),
and the generated code pushes/pops `rbx`, `rbp`, `rsi`, `rdi`.  It keeps `c8`,
`hmap4`, `cxt`, the component pointers and the sums in registers, emits the hash
table lookup (`find`) inline for ICM/ISSE, and uses SSE2 (`pmaddwd`) for MIX.

## Contract

`update()` only updates the components: the `c8`/`hmap4` bookkeeping and the
HCOMP run at byte boundaries stay in C (`zq_pred_update_c8`), exactly like
upstream's `Predictor::update()`.  `zq_pred_init()` frees the code so the next
`predict()` reassembles it for the new model (upstream does the same).

## State mapping

Offsets come from `offsetof(zq_predictor, …)` and `offsetof(zq_component, …)`.
Our structures have upstream's member order and widths (`c8`, `hmap4`, `p[256]`,
`h[256]`, `z`, `comp[256]`, then the tables; and per component `limit`, `cxt`,
`a`, `b`, `c`, then the three arrays, whose first member is the data pointer),
so the generated code addresses them exactly as upstream's does.

## Verification

* `tests/unit/pjit_diff.c` (`make test-e18`) builds random *valid* component
  lists (CM, ICM, MATCH, AVG, MIX2, MIX, ISSE, SSE, 1–8 components per model),
  each predictor gets its own VM, and the two paths are compared after every
  bit: prediction, `p`, `h`, the component counters and the whole `cm`/`ht`/`a16`
  contents.  300 configs × 512 bytes in the suite; 2000 × 4096 (8 M bits) while
  developing, plus 1000 × 1024, all identical.
* The E3 trace (`predictor_dump` + oracle) runs with the JIT on and matches the
  reference; a JIT-enabled build of the oracle produces the same 4647-line trace
  as this port, so the ported emitter is byte-exact against upstream's.

## Known differences (both are upstream's behaviour)

**Repeated `predict()` without `update()`.** `predict()` for ICM/ISSE saves the
bit history in `cxt`, but `update()` re-reads the table entry it has just
rewritten.  If `predict()` is called several times in a row, the entry no longer
matches `cxt` and the JIT and the interpreter disagree — upstream's do too
(verified: an oracle built with `-DNOJIT` and one built with the JIT produce
different traces for 8 predicts followed by 8 updates).  The coder always
predicts, codes and updates one bit at a time, so this is unreachable in
practice; the E3 trace interleaves the calls for that reason.

**32-bit stores into `size_t` fields.** The generated code writes `cxt`, `a`,
`b`, `c` and `limit` with 32-bit stores, leaving the high half as it was (zero
after `init()`).  `pjit_diff` compares only the low 32 bits, like upstream.
For SSE the JIT stores the masked context and the interpreter the unmasked one;
only `cxt & (cm.size()-1)` is ever used.
