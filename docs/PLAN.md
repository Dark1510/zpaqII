# zpaqII — port plan

Port of **zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) to **Pascal (host) + C
(core) + x86-64 assembly (hot paths)**.

Frozen reference: commit `9ab539f` ("Fixes incorrect JIT assembly of \"a+= 128\"...").
Working copy: `/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq` (oracle binary included).

---

## 1. Goal and acceptance criteria

**Goal:** **bit-exact** compatibility with zpaq 7.15.

The project is done when all four properties hold for every case in the matrix:

1. **Extract:** a `.zpaq` produced by upstream extracts with zpaqII byte for byte.
2. **Add:** with the same input, timestamps/attributes, options and `-t`, zpaqII
   produces a **byte-identical** `.zpaq`.
3. **Cross:** each direction is also tested the other way (zpaqII add → upstream
   extract, upstream add → zpaqII extract).
4. **CLI:** same commands/options/observable behavior (messages, exit codes,
   `list`, `-until`, `-index`, multi-part, encryption).

Minimum matrix: `-m0..-m5`, `-method x1..x14,i1..i6,s1..s7` (representative
subset), with and without `-t`, dedup across versions, `-until`, `-key`,
multi-part, UTF-8 names, empty files, binaries, text, already-compressed data,
and inputs larger than one block.

**Thread note:** `-t4` and `-t1` do **not** produce the same bytes in `-m4/-m5`
(verified: same size, different content). Bit-exactness is therefore required
**for the same thread count as upstream**. Reproducing the multithreaded write
order is part of the port (see §5, risk R3).

---

## 2. Architecture

```
                 ┌──────────────────────────────────────────────┐
   CLI / archiver│  Pascal (FPC 3.2.2)  host/                   │
   journaling    │  cli, scan, dedup, index, add/list/extract,  │
   I/O, threads  │  threads, archive encryption, multi-part     │
                 └───────────────┬──────────────────────────────┘
                                 │ C ABI (Reader/Writer callbacks)
                 ┌───────────────┴──────────────────────────────┐
   codec         │  C (gcc 14, -std=c11)  core/                │
   ZPAQL VM      │  sha1/256, aes, state table, zpaql (asm+VM), │
   predictor     │  predictor, arithmetic coder, postproc,      │
   coder         │  config/method, divsufsort, x86-64 JIT       │
                 └───────────────┬──────────────────────────────┘
                                 │
                 ┌───────────────┴──────────────────────────────┐
   hot paths     │  asm/  (GAS, Intel syntax)                   │
                 │  coder, model updates, memcpy/bit ops,       │
                 │  JIT templates                               │
                 └──────────────────────────────────────────────┘
```

Golden rule of the split: **the C core knows nothing about files or threads**;
it only sees streams (`zq_reader`/`zq_writer`), buffers and parameters. The
Pascal host knows nothing about the compressed format; only blocks, segments and
the core API.

### C contract (equivalent to libzpaq.h)

The surface the archiver needs from libzpaq is small (measured with grep over
`zpaq.cpp`):

| API | use in zpaq.cpp |
|---|---|
| `Reader` / `Writer` | file and buffer I/O |
| `StringBuffer` | accumulate fragments before compressing |
| `SHA1` | block/segment checksums and dedup |
| `AES_CTR`, `stretchKey`, `random` | encryption |
| `Decompresser` (`findBlock`, `setInput`, `decompress`, `readSegmentEnd`, `getMemory`, `hcomp`, `pcomp`) | extract/list |
| `compressBlock(StringBuffer*, Writer*, method, filename, comment, dosha1)` | compress one block |
| `Array` | 64-byte aligned buffers |

Everything else (Predictor, ZPAQL, Compiler, divsufsort) is internal to the core.

---

## 3. Stages

Every stage leaves the tree building and the tests green. The ordering aims to
let the oracle (upstream) validate each piece as early as possible.

### E0 — Infrastructure and oracle ✅
- Repo layout, `Makefile`, `docs/` (this plan, ARCHITECTURE, ADRs).
- `tools/oracle/`: upstream build plus instrumented harnesses that dump
  intermediate states (compiled bytecode, state tables, predictions `p`, `H[]`,
  coder output) as stable text.
- Reproducible corpus generator (`tests/corpus.sh`): synthetic files with fixed
  mtimes plus samples from `/mnt/OSR_D3/fileFormatSamples` (copied, never in place).
- `tests/run.sh`: comparison matrix (file hash, `cmp`, exit codes).
- **Acceptance:** `make oracle && make test` runs and fails only because the port
  does not exist yet. *(done)*

### E1 — Deterministic primitives (C) ✅
- `sha1.c`, `sha256.c`, `aes.c` (+ `stretchkey`), `random.c`, `buffer.c`
  (64-byte buffers, zero-filled), upstream-compatible error handling
  (`zq_error` → exit 1 when no frame is installed).
- **Acceptance:** known vectors (NIST/FIPS) plus byte-for-byte comparison against
  the oracle harness for random keys/sizes and `stretchKey` (100k iterations in
  upstream → zpaq's own PBKDF2-SHA256). *(done: 357 cases identical)*

### E2 — ZPAQL VM (C + asm)
- `zpaql_asm.c`: ZPAQL text → bytecode compiler (`OPCODES`/upstream table,
  `Compiler::compile`), including `pcomp` and comments.
- `zpaql.c`: `run0`/`run` interpreter with exact semantics (A/B/C/D/R/H/X,
  jumps, `HASH`, `OUT`, `HALT`, 32-bit wrap, `A<<=X` with `X&31`, etc.).
- `asm/zpaql_jit_x86_64.S` + `jit.c`: per-block translation to machine code
  (equivalent to `ZPAQL::assemble`), with an interpreter fallback (`NOJIT`).
- **Acceptance:** for every config of `-m0..-m5` and `-method ...`, compiled
  bytecode is identical to the oracle; HCOMP/PCOMP execution matches `H[]`,
  outputs and timings. Differential: interpreter ≡ JIT over 10^6 random steps.
- **Status:** compiler + interpreter ✅ (26 configs, bytecode and traces
  identical). JIT pending.

### E3 — Predictor and coder (C + asm)
- `state.c` (StateTable), `component.c` (CONS/CM/ICM/MATCH/AVG/MIX2/MIX/ISSE/SSE),
  `predictor.c` (init/update/predict, hmap4, c8, nibble boundaries, `MATCH` buffer).
- `coder.c`: `Decoder` and `Encoder` (carryless range coder), `decode(0)` = EOS.
- `asm/coder_x86_64.S` and `asm/model_x86_64.S`: inner loops (see §4).
- **Acceptance:** per-bit trace of `p` and `c0` identical to the oracle on all 5
  levels; decode oracle blocks bit for bit; encoded streams byte-identical.
- **Status:** predictor (all 9 component types) + coder ✅ — 13 models compared:
  512-byte prediction traces (4608 byte traces), encoded stream bytes and
  decoder round trip, all identical to the upstream interpreter. ASM kernels
  pending.

### E4 — Container and high-level API (C)
- `decompresser.c` (findBlock, headers, HCOMP/PCOMP, SHA-1 verify, memory),
  `postproc.c` (lazy2, E8E9, BWT) — the level-2 BWT is handled by a pcomp program
  (`bwtrle`).
- `compressor.c` (`compressBlock`), `config.c` (`makeConfig` text for every
  level/method), `method.c` (`x/i/s/0` parsing + args), `divsufsort.c` (port of
  the embedded one, plain C, unchanged semantics).
- **Acceptance:** round-trip of all 5 levels + bit-identical `compressBlock`
  against upstream over the corpus; corrupted blocks must fail SHA-1 the same way.
- **Status:** ✅ both directions.
  - Extract: 38 upstream streaming archives (store, LZ77, BWT, E8E9, multi-block,
    empty input) extracted identically with per-segment SHA-1 verification.
  - Compress: 51 single blocks compressed bit-exactly against upstream
    (levels 0–7 defaults, `x` methods with LZ77/SA/BWT/E8E9, all component
    types, empty input).
  - `makeConfig` produces textually identical configs for the tested methods;
    the fixed postprocessor programs are generated from upstream by
    `tools/gen_config.cpp` into `src/core/config_tpl.c` (checked in, like the
    predictor tables).

### E5 — Archiver (Pascal)
- `fio.pas` (file layer, dates/attributes win/unix), `archive.pas`
  (Input/OutputArchive, multi-part, seeks), `scan.pas` (scandir, symlinks,
  `-not`, `-only`), `readarc.pas` (JIDAC index parsing: `jDC\x01`, fragment
  blocks), `add.pas` (fragmentation, SHA-1 dedup, journaling, versions,
  `-until`), `extract.pas`, `list.pas`, `index.pas`, `cli.pas`, `threads.pas`
  (pool + semaphores with the same ordering policy as upstream).
- **Acceptance:** full E6 matrix over commands and options, including `-key`,
  multi-part, `-until`, `-fragment`, `-index`, and error/exit-code behavior.
- **Status:** streaming mode (`-method s...`) done: `zqcore.pas` (C bindings),
  `zqfile.pas` (dates, attributes, paths), `zqarchive.pas` (multi-part + AES-CTR
  reader/writer) and `zpaqii.lpr` (`add`/`extract`/`list`). 23 streaming archives
  (single/multi file, multi-block, empty, E8E9, BWT, all model types) are
  **bit-identical** to upstream and extract in both directions.
  - JIDAC reading (`zqjidac.pas`): transaction/fragment/index blocks, versions,
    `-until`, `list`, and `extract` of upstream journaling archives at all levels
    (0–5), including multi-version rollback, verified against upstream's own
    extraction.
  - Journaling `add` (fragmentation, dedup, index writing) is **not implemented
    yet**: the CLI rejects numeric methods with a clear message. The algorithm
    and its bit-exactness requirements (sort keys, rolling hash, HTIndex,
    block/method selection, h/i/c blocks) are documented in the plan and in
    the upstream reference.

### E6 — Full verification and performance
- `tests/matrix.sh`: cross matrix, both directions × levels × methods × options.
- Differential fuzzing of the decoder (truncated/corrupt archives: same error
  message and exit code as upstream, no crashes, no leaks).
- Benchmarks: identical ratio (guaranteed by bit-exactness) and speed target
  ≥ 90 % of upstream for `-m1/-m2`; `perf` analysis of the asm loops.
- Sanitizers: `-fsanitize=address,undefined` on the C core; `-Cr -Co` on Pascal
  for CI (never for release binaries).
- **Acceptance:** 100 % green matrix, no warnings with `-Wall -Wextra`,
  valgrind/ASan clean when decoding the corpus.

---

## 4. Where the assembly goes (and why)

| Routine | Why | Implementation |
|---|---|---|
| `Decoder::decode` / `Encoder::encode` (byte-level) | ~1 op per compressed byte, time-dominant | inline asm in `coder.c` + C fallback |
| ICM/ISSE/MIX/SSE/CM updates (`p[i] += ...`, `dt[]` lookups) | ~8 models per bit | block asm `asm/model_x86_64.S` |
| `ZPAQL` HCOMP/PCOMP | runs once per input byte | JIT that emits code (equivalent to `ZPAQL::assemble`) |
| 64-byte aligned `memcpy`/`memset`, `bswap`, `popcnt/lzcnt` | hot accesses | asm with CPU detection (`cpuid`) |
| Optional: SHA-1 with SHA-NI | fragment dedup | conditional asm |

Rule: **assembly never changes semantics**. Every kernel has a differential test
against the pure C version (same function built without asm), and the JIT against
the interpreter. `make NOJIT=1` must produce the same output (only slower).

---

## 5. Risks and mitigations

- **R1 — UB in the original C++.** Upstream does signed shifts, signed `char`,
  aliasing and 32-bit wrapping arithmetic. The port must pin these behaviors
  explicitly (`uint32_t`, `U8`, masked shifts). Mitigation: per-byte differential
  tests and documented wrap helpers in C.
- **R2 — JIT.** The JIT is an optimization, not semantics; the interpreter can
  ship first. Upstream uses it by default, but bit-exactness does not depend on
  it. Mitigation: JIT vs interpreter differential in E2 before enabling it.
- **R3 — Multithreading.** Block write ordering with `-t>1` affects the bytes.
  Mitigation: port `CompressJob`/`writeThread`/`Semaphore` policy as-is; verify
  `-t1` first, then `-t2/-t4/-t8` with the same input.
- **R4 — Memory.** High levels use hundreds of MB per thread (upstream reserves
  and touches pages). Mitigation: same reserve/zeroing strategy, tests under
  `ulimit`.
- **R5 — Date/attribute formats.** Windows vs unix (`decimal_time`, MSDOS
  attributes). Mitigation: fix `-Dunix` like upstream on Linux; port the table as-is.
- **R6 — Corpus size.** The `archive` corpus is 59 GB. Mitigation: copy small
  samples filtered by size into `/mnt/IA_LAB/agentes/zpaqII/corpus`.
- **R7 — Scope.** 11k lines of C++ cannot be ported in one sitting without
  errors. Mitigation: stages with binary acceptance criteria, and the oracle as
  a safety net on every commit.

---

## 6. Repository conventions

- `core/`: C11 + asm only; no external dependencies; no file I/O.
- `host/`: Pascal only; no compression logic.
- Naming: `zq_*` for the core, `T*` for Pascal classes.
- One commit per module with its test; never leave the tree unbuildable.
- English only in code, comments, docs and commit messages.
- License: Unlicense (same as upstream); `divsufsort` keeps its MIT license.
