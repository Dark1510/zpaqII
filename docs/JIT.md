# ZPAQL JIT — design and status

Status: **implemented and verified**. `zq_zpaql_assemble()` is a direct port of
upstream's `ZPAQL::assemble()` (libzpaq.cpp 3033–3812), byte for byte; the
generated code is executed from `zq_zpaql_run()` and every build falls back to
the interpreter when no code was produced.

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
3. OUT block (a local subroutine): `outbuf[bufptr++] = a`, and when the buffer
   fills, call `zq_jit_flush()` with the registers saved.
4. `start`: save registers, load the state, then the translated program.
5. A second pass patches every jump target.

Code lives in an `mmap`ed RWX page (`zq_allocx`); it is freed whenever the
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

## Next step: `Predictor::assemble_p`

The predictor loop (`Predictor::assemble_p`, `pcode`) is a separate JIT and is
not ported yet; `make bench` shows it is now the main remaining gap at `-m5`
(add 60–77 %, extract 60–74 % of upstream).
