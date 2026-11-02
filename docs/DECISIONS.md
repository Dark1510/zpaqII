# Decisions (ADRs)

## ADR-001 — The oracle is the binary, not the documentation

**Decision:** the reference is compiled zpaq 7.15 (commit `9ab539f`) and the
criteria are binary: file `cmp`, trace diffs, exit codes. The prose spec is not
treated as ground truth when it contradicts the binary.

**Consequence:** every stage adds a harness that instruments upstream
(`tools/oracle/`) plus a differential test. A module is done only when the diff
is empty. This is what revealed, for example, that `-t1` and `-t4` do not produce
the same bytes for `-m4/-m5`.

## ADR-002 — Pascal / C / asm split

**Decision:** Pascal (FPC) for CLI, archiver, journaling, dedup, threads and file
I/O; C11 for the codec (ZPAQL VM, predictor, coder, hashes, postprocessors);
x86-64 asm for hot paths and the JIT. Single boundary: `include/zq.h`.

**Rationale:** the codec needs exact type and arithmetic control (bit-exactness)
and benefits from asm; the archiver is state logic with strings, threads and
files, where Pascal is far more productive. The core knows nothing about files or
threads: only `zq_reader`/`zq_writer` callbacks.

## ADR-003 — Error unwinding

**Decision:** `zq_error()` copies the message and `longjmp`s to a frame installed
by the core entry point (thread-local). The host never sees a `longjmp`: every
core function returns a status and `zq_last_error()`.

**Rationale:** upstream throws `std::runtime_error`/`bad_alloc` and `main` prints
it; unwinding through Pascal frames with `longjmp` is not safe. All jumps happen
between C frames.

## ADR-004 — Threads and bit-exactness

**Decision:** bit-exactness is required for the same `-t` as upstream. Block write
ordering in `-m4/-m5` with `-t>1` depends on the scheduler; the port must
reproduce the same policy (`CompressJob`, `writeThread`, `Semaphore`), not just
the format.

**Verification:** `-t1` matrix first, then `-t2/-t4/-t8` comparing bytes with
upstream.

## ADR-005 — Tolerated divergences

- A corrupt `.zpaq` produces a clean error message (exit 1) where upstream
  sometimes aborts on `assert`. This is recorded in `docs/DEVIATIONS.md` as cases
  appear; the priority is to never crash or corrupt output.
- ZPAQL execution error texts differ ("Bad ZPAQL opcode" from the JIT vs "ZPAQL
  execution error" from the interpreter), but the exit code and the failure point
  match.
