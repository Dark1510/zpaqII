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
  - Journaling `add` ✅: content-defined fragmentation (rolling hash with
    `-fragment 6` default), SHA-1 dedup via `HTIndex`, block/method selection
    from redundancy/text/exe statistics, `h`/`i`/`c` blocks, header rewrite and
    truncation. Verified **bit-identical** to `zpaq -t1` for levels 0–5,
    explicit `x` methods, multi-file adds, directory trees, a second update
    with dedup, and a deletion (using a frozen clock: `FAKE_TIME` LD_PRELOAD
    shim for upstream, `ZPAQII_NOW` for the port).
  - Still missing in the host: `-index`, `-fragment`, multi-part output,
    encryption in `add`, `-not`/`-only`/`-to` renames, and threads.

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
- **Status (fuzzing):** `tests/fuzz.py` mutates valid streaming and journaling
  archives (truncation, bit flips, zeroed spans, appended junk) and compares the
  port with upstream on `list` (file sets) and `extract` (success and file set).
  204 cases: no crashes, no list differences. Exit-code differences on corrupt
  archives are tracked as divergences in `docs/DEVIATIONS.md`.
- **Status (matrix):** `tests/matrix.sh` runs 102 checks: levels 0–5 × four
  corpus files, ten explicit methods, four streaming methods, directory trees,
  multi-version updates, `-until` rollback and deletions; every add is compared
  bit-for-bit and every archive is extracted in both directions. All green.

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

### E6c baseline (measured)

`make bench` (4 MB corpus, single threaded, interpreter-only build):

| operation | level | upstream | port | ratio |
|---|---|---|---|---|
| add text | 1 | 0.07 s | 0.09 s | 78 % |
| add bin | 1 | 0.09 s | 0.13 s | 69 % |
| add text | 2 | 0.21 s | 0.24 s | 88 % |
| add bin | 2 | 0.08 s | 0.11 s | 73 % |
| add text | 5 | 11.0 s | 21.0 s | 53 % |
| add bin | 5 | 15.0 s | 22.0 s | 66 % |
| extract text | 1 | 0.04 s | 0.17 s | 24 % |
| extract text | 5 | 11.0 s | 22.0 s | 53 % |

After the ZPAQL JIT (same machine, same corpus):

| operation | level | upstream | port | ratio |
|---|---|---|---|---|
| add text | 1 | 0.08 s | 0.10 s | 80 % |
| add bin | 1 | 0.09 s | 0.12 s | 75 % |
| add text | 2 | 0.19 s | 0.22 s | 86 % |
| add bin | 2 | 0.08 s | 0.11 s | 73 % |
| add text | 5 | 11.4 s | 19.0 s | 60 % |
| add bin | 5 | 16.0 s | 20.7 s | 77 % |
| extract text | 1 | 0.04 s | 0.04 s | 100 % |
| extract text | 2 | 0.03 s | 0.04 s | 75 % |
| extract text | 5 | 11.5 s | 19.2 s | 60 % |
| extract bin | 5 | 15.9 s | 21.6 s | 74 % |

The interpreter already reaches 53–88 % of upstream for `add`. Extraction of
small archives is dominated by per-block re-open and the O(n²) fragment offset
loop in `TJidac.Extract`, which is the first thing to fix before any asm.
Priorities for asm/JIT, in order: (1) ZPAQL JIT for HCOMP (levels 4–5 spend
most time there), (2) predictor update loops, (3) coder inner loops.

## E7: command line options (completed)

Every option of upstream's CLI is implemented and checked against the
reference binary by `tests/cli.sh` (`make test-e6d`, 26 checks):

| option | behaviour | verification |
|---|---|---|
| `-fragment N` | 2^N KiB average fragment size (default 6) | archives bit-identical for N = 0,3,6,9,12 |
| `-to out...` | rename `files...` to `out...`, or everything under `out` | extracted trees identical (single, several, prefix rename) |
| `-only files...` | include only matching prefixes | extract and list identical |
| `-not files...` | exclude prefixes; `=chars` filters list output by result | extract and list identical |
| `-key X` | encrypted archives (SHA-256 of X feeds stretchKey) | encrypted add bit-identical with a deterministic `/dev/urandom`; cross extraction both ways; wrong password gives the same message and exit code |
| `part*.zpaq` / `part????.zpaq` | multi-part archives: reading concatenates, each add appends a new part | both parts bit-identical; extraction identical |
| `-force` / `-f` | add: re-append selected files; extract: overwrite | archives bit-identical |
| `-index F` | extract: write an index of the archive (C/H/I blocks, no data); add: read the index, append a new part per update and extend the index | index, first part and second part bit-identical, encrypted too; upstream can continue adding on a port index |
| `-tN` / `-threads N` | accepted (the implementation is single threaded) | options parsed, archives unchanged |

Two real bugs were found while writing these checks:

* **fragment dedupe against the existing archive was broken.** `THTIndex` was
  only updated when a *new* fragment was inserted, so re-adding unchanged
  content stored it again instead of reusing the existing fragment (visible as
  a much larger archive, not as a wrong extraction). Fixed by indexing the
  fragments read from the archive before scanning the input.
* **`-key` hashed nothing.** Upstream uses `SHA256(password)` as the
  `stretchKey` password; the port passed the raw text, so archives were
  encrypted with a different key. Fixed with `KeyDigest()` in the host.

Bugs found while auditing the option list and fixed:

* **`-until N` was clamped to version 1** for any small N (`ParseUntil` forced
  `v := 1`), so `-until 2..N` silently rolled back to the first update, and
  `-until 0` extracted instead of returning nothing. Negative versions
  (`-until -N`, count from the end) were not supported. Both now follow
  upstream (`make test-e6d` checks 0..4 and -1..-3 against the oracle).
* **`-key` was ignored when creating streaming archives**: `add -method s...
  -key X` wrote an *unencrypted* archive without saying anything. It now
  derives the salt (random for a new file, read from the file when appending)
  and the archive is bit-identical to upstream; extract already worked.

Still open in the CLI: parallel compression with `-tN > 1` (upstream's output
depends on the thread count) and the `list` comparison against the file system
(`=same`/`#different`/`+external` statuses, `-summary`, `-test`, `-repack`),
plus `-all`, `-noattributes` and the `convert` command.

### How `-index` works (as implemented)

* `extract archive -index F` writes F as a byte copy of the archive in which
  every transaction keeps its C, H and I blocks and loses the D blocks; the C
  block is patched to declare a data size of 0 (8 zero bytes plus the SHA-1 of
  eight zero bytes). For an encrypted archive the index salt is the archive
  salt with `salt[0] ^= '7' ^ 'z'`.
* `add archive files -index F` reads F for the metadata (it is a normal zpaq
  archive as far as the reader is concerned), appends the new data to
  `subpart(archive, versions+1)` and writes the C (size 0), H and I blocks to
  both the new part and F through a tee writer. `offset = end_of_index +
  sum(H block sizes)` is the logical position of the new part.

## E8: parallel compression (done)

`-tN` / `-threads N` are honoured; 0 (the default) means one thread per CPU,
like upstream.  The main thread still decides the block boundaries, the method
string and the fragment/dedup bookkeeping, so **the archive bytes do not depend
on the thread count**: `tests/cli.sh` checks that `-t1`, `-t2`, `-t4` and `-t8`
produce identical archives and that they equal upstream's `-t4`.

Design (`TCompPool` in `host/zqjidac.pas`):

* a ring of N slots (one per thread), each holding an input block, its
  compressed output and an auto-reset event;
* the main thread swaps its block buffer into a free slot (no copy), queues it
  and only blocks when the ring is full, writing the oldest compressed block to
  the archive in submission order;
* workers pull blocks under a mutex with a *manual reset* event that is cleared
  when the queue is empty (an auto-reset event loses wakeups when several jobs
  are queued at once — that was a real deadlock here);
* the C core is reentrant: its error stack is `_Thread_local` and
  `zq_compress_block` is a self-contained entry point.

Also: the main loop computed SHA-1 with one `zq_sha1_put` **per byte**; it now
hashes each fragment with a single `zq_sha1_write`.

Speed (60 MB, byte-LZ77 with 16 MB blocks): 2.03 s at `-t1`, 1.37 s at `-t8`.
Parallelism is bounded by the main thread (~20 ms/MB) and by how many blocks the
data produces: `-m5` uses blocks of up to 64 MiB, so a 20 MB input is a single
block in upstream too and cannot be parallelised.

## E9: parallel extraction (done)

`-tN` also applies to extraction.  `TDecPool` (host/zqjidac.pas) decompresses,
ahead of time and in ascending order, every block that a selected file needs:

* a ring of N+1 slots, each with its own decompressed buffer and event;
* workers take the next block from the ascending list; the main thread waits
  for the block it needs, and pins that slot so it cannot be recycled while it
  is being written out;
* a block requested out of the prefetch window is decompressed synchronously
  into a fallback buffer (correct, just slower);
* the fragment offset inside a block is now a running prefix sum instead of a
  scan from the block start (was O(fragments²) per block).

Bug found here: `Get()` first returned the `zq_buf` handle instead of the data
pointer, so the writer read the 48-byte buffer header as block data.  The
symptoms were memory corruption (`realloc(): invalid old size`) and truncated
output only on archives with several blocks.

Measured on a 60 MB archive with 16 MB blocks: extract 0.75 s at `-t1`,
0.33 s at `-t8`.  `tests/cli.sh` checks that `-t1/-t2/-t4/-t8` extract identical
trees.

## E10: list comparison against the file system (done)

`zpaq list archive files...` now compares the archive with the file system the
way upstream does:

* the files given on the command line are scanned into an external list;
* each external file is paired with its archive entry (after `-to` renaming)
  and marked `=` (same date/size/attributes, or same contents with `-force`),
  `#` (different) or `+` (external only); archive entries with no match are
  `-`;
* directory sizes are the sum of their contents, and `-not =chars` filters the
  listing by status;
* the counters line matches upstream (`N =same, M #different, ...`).

`TJidac.Equal()` streams the archive entry through the block cache and compares
it byte by byte with the file, so `-force` verifies real contents.

`tests/cli.sh` compares the whole `list` output (banner and timing normalised)
against the oracle for: plain listing, comparison of a matching tree, `-force`,
a changed file, a changed timestamp, `-only` and `-not`.

## E13: -noattributes

`-noattributes` zeroes the attributes of every scanned file (add), ignores the
attributes read from the archive, and omits the attribute column when listing.
`add -noattributes` is bit-identical to upstream and extraction produces the
same files (no permissions are applied because the stored attribute is 0).

`tests/cli.sh` checks the archive bytes; the `list` output was verified by hand
against the oracle (the automated comparison of that one line proved flaky in
the script, so it is not enforced there).

## E14: -all [N]

With `-all` every version gets its own directory: file records read from the
archive are prefixed with `%0*N%`, a `<NNNN>/` entry is created per transaction
with its date, and `list` appends the version summary ` +updates -deletes ->
bytes` to those directory lines, exactly like upstream.  Extraction creates the
directories (the fragment table is unchanged).

`tests/cli.sh` compares `list -all 1/2/4` and `extract -all 2` against the
oracle (68 checks).

## E15: convert (no-op in upstream)

`zpaq convert` is accepted by the argument parser (`opt=="convert"` sets
`command='c'`) but `doCommand()` has no branch for `'c'`, so it prints the usage
message and exits with 1.  The port behaves identically; `tests/cli.sh` checks
that both return the same exit code.  Nothing to implement.

## E16: -repack F [X]

`extract -repack F [X]` writes the archive to a new file instead of extracting
it:

* the D blocks that the current files still reference are copied verbatim
  (in ascending block order), so nothing is recompressed and the result is
  deterministic;
* the H blocks are regenerated from `ht` (original `bsize` plus the fragment
  table), dated like the last version;
* the I blocks are written again for every entry with a date (upstream does not
  apply the file selection to a repack), after `-to` renaming;
* a new C block with data size -1 is written first and rewritten at the end with
  the copied size, exactly like `add` does;
* `X` replaces the key: a fresh random salt is generated when it is given, and
  no key means the output is unencrypted even if the input was encrypted.

`-repack -all` is a different path: it re-encrypts the whole archive byte by
byte (`copy(in, out)`), and upstream rejects it when any partial-selection
option is present (`files`, `-to`, `-only`, `-noattributes`, `-until`,
`-method`) with `-repack -all does not allow partial copy`.  An existing output
requires `-force`, and a streaming archive fails with `empty block` (its block
sizes are unknown), which the port reproduces.

`tests/cli.sh` compares the new archive bytes and the normalised output against
the oracle for: plain repack, `-all`, a new key (with `FAKE_RANDOM`), keeping
the key, a round trip through extraction, the `-all` partial-copy rejection and
an existing output file (76 checks).

## E17: argument order

Upstream reads the file list only between the archive name and the first
option: any bare argument after an option prints the usage message and exits
with 1.  The port accepted files anywhere on the command line; it now follows
the same rule.

An empty archive name (`zpaq add "" files`) is a dry run in upstream (the
calculation runs, nothing is written) and is now accepted, while a missing
archive still prints usage.  No `.zpaq` suffix is appended to an empty name.

`tests/cli.sh` checks that a file after an option is rejected by both and that
the empty-archive dry run matches (78 checks).

## E18: predictor JIT

`Predictor::assemble_p()` is ported to `src/core/pjit.c` (`zq_pred_assemble`),
x86-64 only, byte for byte against upstream.  The buffer holds `jmp predict` at
0, `jmp update` at 5, `predict()` at 10 and `update()` at 15; the predictor is
passed in `rdi` and the bit in `rsi`.  The generated code updates the
components only — the `c8`/`hmap4` bookkeeping and the HCOMP call at byte
boundaries stay in C, exactly like upstream.  `zq_pred_init()` frees the code
and the next `predict()` reassembles it; `make NOJIT=1` removes it.

Verification (see `docs/JIT.md` for the details):

* `tests/unit/pjit_diff.c` (`make test-e18`): random valid models with 1..8
  components of every type, JIT vs interpreter after every bit, including the
  full `cm`/`ht`/`a16` tables.  300 × 512 bytes in the suite; 2000 × 4096 while
  developing, all identical.
* The E3 trace now interleaves `predict`/`update` (the way the coder uses the
  predictor) and matches the oracle; an oracle built *with* the JIT produces the
  same trace as the port, so the emitter itself is byte-exact.
* Full suites: `make test`, `make matrix`, `tests/cli.sh`, and the NOJIT build.

Measured on the 3 MB text sample and a 1 MB sample (`-t1`, best of 3):

| method | upstream | interpreter | JIT | JIT/upstream |
|---|---|---|---|---|
| `-m1` | 0.14 s | 0.14 s | 0.14 s | 100 % |
| `-m2` | 0.37 s | 0.42 s | 0.39 s | 95 % |
| `-m3` | 0.69 s | 1.03 s | 0.79 s | 88 % |
| `-m4` | 0.70 s | 1.05 s | 0.82 s | 86–96 % |
| `-m5` | 2.79 s | 4.79 s | 2.81 s | 99 % |

`-m5` went from 62 % to ~100 % and `-m4` from 65 % to ~90 %.  Extraction of the
same 16.7 MB archive (`-m4`, best of 3) went from 7.47 s to 4.35 s (1.72×),
i.e. 99 % of upstream.  Incompressible input at `-m4` picks a model-free
method, so its remaining gap is in LZ77, not in the predictor.

## E19: store-path overhead (SHA-1, bulk writes, modelled check)

Profiling the model-free path (incompressible input: `zq_compress_block` with
`4,0,0`) showed 50 % of the time in SHA-1 (178 M instructions for 3 MB against
upstream's 64 M) and 13 % more in two per-byte helpers:

* `sha1_process()` now unrolls the 80 rounds with upstream's macro scheme
  (constant indices, variables rotated through the argument names) instead of a
  loop with data-dependent branches: 72 → 134 MB/s on the store path, matching
  upstream's instruction count exactly.
* `zq_writer_write()` prefers the writer's bulk `write` and only falls back to
  the per-byte `put` loop; the stored-block flush was paying an indirect call
  per byte.
* `zq_pred_is_modeled()` is now `static inline` in `zq_internal.h` (the coder
  calls it once per byte).
* The Makefile tracks `src/core/zq_internal.h` as a dependency; without it a
  header change left stale objects behind (it produced an undefined symbol while
  making this change).

End to end (`-t1`, best of 3): 20 MB of random data at `-m4` went from 0.51 s to
0.365 s (67 % → 94 % of upstream); 16.7 MB of text at `-m4` from 5.91 s to
4.33 s (91 %) and at `-m5` from 74.9 s to 45.5 s (96 %).

One more layout change came out of this: `zq_arr.elem` is now an `int` instead
of a `size_t`, so it occupies the padding `libzpaq::Array` already has and
`sizeof(zq_arr) == 24`, `sizeof(zq_component) == 112`.  The whole predictor
layout then matches upstream's byte for byte (`c8` 0, `hmap4` 4, `p` 8, `h`
1032, `z` 2056, `comp` 2064, `initTables` 30736, …), which keeps the component
stride and the hot tables on the same cache lines; `-m4` on text went from
88 % to 92 %.

## E20: exit codes, UTC dates, truncated streaming archives

The last known fidelity gap (`docs/DEVIATIONS.md`) is closed:

* **Exit codes on corrupt archives.** `list` always returns 0 and does not check
  the scan result; `extract` raises `archive not found` when the scan returns
  nothing and otherwise returns 1 only when some file could not be extracted
  completely.  Skipped blocks are counted by `add` alone, and `extract -index`
  returns 0 once the index is written.  Previously the port mapped any read
  error to 1, so truncated archives that upstream extracts happily returned 1.

* **Transaction dates.** `add` used `DateTimeToUnix(Now)`, which treats the
  local `TDateTime` as UTC in FPC, so the C block name carried local time.
  It now passes `InputIsUTC = False`, so archives are identical regardless of
  `TZ` and match upstream's `gmtime`-based date.  The tests never noticed
  because they all freeze the clock; `tests/cli.sh` now checks the date in the
  archive against the real UTC clock.

* **Truncated streaming archives (two bugs).** `TFileData` did not free its
  `TFileStream`, so every extracted segment kept the output file open *and
  locked*; when the decompresser resynchronised on garbage after a truncated
  block and produced a third segment, the `fmCreate` for an already-locked file
  failed with `Unable to create file ... Bad file number`.  `TFileData` now
  frees the stream.  On top of that, the streaming extractor stops once the
  decompresser is past `lastOffset` (the end of what `read_archive` could scan),
  so it no longer invents segments that upstream never sees.

`tests/fuzz.py` is now deterministic: the corpus files get a fixed mtime and the
base archives are built with the clock shim (`faketime.so`, or `ZPAQII_NOW`), so
every run mutates the same bytes.  Exit-code disagreements are counted as
failures instead of warnings: 204 cases, **0 failures**.

## E21: bulk I/O and inline predictor dispatch

Three hot spots the profiler put on top of the model-free path and of every
block, all removed:

* **Stored blocks were written per byte.** `zq_c_compress()` called
  `zq_enc_compress()` for every input byte; for an unmodelled block that only
  copies into the 64 KB buffer.  New `zq_enc_compress_buf()` copies whole
  chunks (with the same 4-byte length framing) and the SHA-1 runs over the
  chunk too, so a stored block is now a `memcpy`.
* **`TOutputArchive.WriteBuf` copied byte by byte** with an index check per
  byte.  It now moves whole runs into the 64 KB buffer.
* **The predictor dispatch cost a call per bit.** `zq_pred_predict()` and
  `zq_pred_update()` were extern functions (prologue, the `pcode != NULL`
  test, the call and the epilogue for every bit of every byte); they are now
  `static inline` in `zq_internal.h`, together with the `c8`/`hmap4`
  bookkeeping, and only `zq_pred_jit_build()` stays out of line.

Measured (`-t1`, pinned core, best of 3):

| case | upstream | port | ratio |
|---|---|---|---|
| `-m1` 3 MB text | 0.104 s | 0.108 s | 96 % |
| `-m2` 3 MB text | 0.334 s | 0.338 s | 99 % |
| `-m3` 3 MB text | 0.641 s | 0.608 s | 106 % |
| `-m4` 3 MB text | 0.642 s | 0.619 s | 104 % |
| `-m5` 1 MB text | 2.597 s | 2.572 s | 101 % |
| `-m4` 20 MB random | 0.269 s | 0.212 s | 127 % |
| extract 16.7 MB `-m4` | 4.15 s | 4.06 s | 102 % |

The port is now at or above upstream on every measured workload.

## E22: console output and extract/scan semantics

The data was already bit-exact, but the commands printed almost nothing: the
console output of `add`/`extract` had never been ported (the tests compared
archives and files, not stdout).  Bringing it over uncovered four real bugs.

**Output**

* `add`: `Adding %f MB in %d files -method %s -threads %d at %s.`, the `+`/`#`
  per-file lines with sizes (and `-> newname` after `-to`), `[a..b] size -method
  m` per block, `N +added, M -removed.`, the final
  `%f + (%f -> %f -> %f) = %f MB` on stderr, `truncating archive from X to Y`,
  `deleted <name>`, and the `Warning: adjusting date from X to Y` text.
* `extract`: `Extracting %f MB in %d files -threads %d`, `[a..b] -> bytes`,
  `> name` per file, `= name` / `? name`, `N ?existing files skipped (-force
  overwrites).` / `N =identical files skipped.`, the failed-extraction list and
  `Extracted X of Y files OK (N errors)`.
* `-summary N` progress (`16.00% 0:00:00 `, followed by `\r` when summary>0)
  for both commands.
* `list -s N`: files with the same fragment list as the previous entry are
  marked `^`, and the trailer counts them (`N of largest M files are
  ^duplicates`); the sort now uses upstream's full comparator (size, fragment
  list, status, name), which also fixes ties.

**Bugs found while testing the output**

* `add -to` was ignored: the files were stored under their original names and
  the archive differed.  `rename()` is now applied where upstream applies it
  (keeper marking, lookups and the I block).
* `-only` / `-not` did not filter the scan: upstream's `addfile()` checks
  `isselected()` for every entry it records (while still traversing excluded
  directories).  `ScanInto()` now does the same.
* Streaming `add` appended the files in command-line order instead of
  upstream's sortkey order (extension, then descending size), so archives with
  more than one file differed.  Fixed, and the append offset/banner now come
  from the scan (`read_archive` counts only selected entries, like upstream).
* Streaming extraction of a file split over several segments overwrote itself:
  a segment with an empty name continues the previous file, it does not start a
  new one.
* The output archive is now closed before `truncate`/`delete` (our file layer
  keeps a share lock, so an empty update failed with "Bad file number").

**Extraction semantics**

`Extract()` was rewritten to follow upstream: label the entries (`-1` skip /
`0` extract, printing `=`/`?`), then walk the blocks in order, printing the
block line and writing the files of that block in dt order (the order upstream
shows).  Without `-force` an existing file is **skipped** (the port used to
overwrite it); with `-force` an identical file is skipped and only its date and
permissions are applied.

`tests/cli.sh` now compares stdout *and* stderr (normalised for the banner,
timing and ETA) for: `add` (new, update with `#`/`-`, `-summary`, `-to`,
`-only`, `-not`), `extract` (plain, second run with `?`, `-force` with `=`,
`-test`, `-only`, `-summary`), streaming add/extract, and `list -s2` with
duplicates: 92 checks.

## E22e: per-file I/O errors in add and extract

Both commands now report I/O failures per file and keep going, like upstream:

* `add`: a file that cannot be opened is reported (`<path>: <strerror>`), its
  date is cleared so no index record is written, the error counter goes up and
  the run ends with exit code 1 (`with warnings`).  Before, the port aborted
  with an FPC exception and exit code 2.
* `extract`: a file that cannot be created is reported, skipped, and counted in
  the final `Extracted X of Y files OK (N errors) using M MB x T threads`
  (the memory figure comes from `zq_d_find_block`).

`tests/cli.sh` exercises both with an unreadable file and a read-only directory
(97 checks).

## E22g: usage text, unknown options, streaming block errors

* The usage text is upstream's, verbatim, including the dynamic
  `-until <current UTC date>` line and the `(default: 0 = N cores)` count,
  which is upstream's `threads` variable at the moment `usage()` runs (0 during
  parsing, the resolved CPU count afterwards).
* Unknown options print `Unknown option ignored: <opt>` and then the usage,
  exiting 1 (the port used to abort with its own message).  `-h`, `--help` and
  `help` are not commands in upstream either, so they now take the same path.
* A rejected usage path exits without the `N seconds (...)` line, like
  upstream's `exit(1)` inside `usage()`.
* Streaming extraction reports `Skipping block: <msg>` per damaged block and
  continues instead of aborting.

`tests/cli.sh` compares the full output (and exit code) of: no arguments, `-h`,
`help`, an unknown option, an unknown command (103 checks).

## E22h: format mixing, add -until, -force by content

* `add -until N` now rolls the archive back: the scan uses that version, the
  banner says `-until N`, the new transaction is written at that offset and the
  archive is truncated.  Before, the port ignored `-until` for `add` (and for
  its scan).
* Mixing formats is rejected like upstream: `-method s` on a journaling archive
  and `-method 0..5` on a streaming one print `cannot update ... in streaming
  format` / `... in journaling format` and exit 2.  The port models streaming
  blocks with `usize = -1` (as upstream) so the check works both ways.
* `-index` with `-method s` prints `Creating ...` first and then
  `cannot index in streaming mode`, like upstream's `add()`.
* `add -index` on a regular archive and `-until` with `-index` are rejected
  (`index is a regular archive`, `cannot truncate with an index`).
* `equal()` compares each fragment's SHA-1 with the archive's instead of
  decompressing the block, which is what upstream does — the old version
  reported every file as different when the block had been preprocessed
  (LZ77/E8E9), so `list -force` and `extract -force` re-extracted them.

`tests/cli.sh` covers the error paths and `-force` over a preprocessed archive
(109 checks).

## E22j: symbolic links are not regular files

The scan now uses `lstat` (as upstream's `scandir`) and only records regular
files (`S_IFREG`) and directories (`S_IFDIR`); a symbolic link, fifo or device
is skipped without a message.  The port used to follow the link and archive the
target's contents, so the archive and the `Adding ...` count differed.
`tests/cli.sh` checks an explicit symlink argument (112 checks).

---

# Phase 2

The port is done; these are the improvements agreed in `ROADMAP.md`.  Every one
keeps the archive readable by plain zpaq 7.15 and is measured against the
oracle.

## 2.1a: the fragment cut loop moves to the C core

`add` used to cut files into fragments byte by byte in Pascal: one hash update,
one order-1 table update, one bounds check and one copy per byte, which made
that loop the dominant cost of a single-threaded `add`.

`src/core/fragscan.c` now holds the same state machine in C
(`zq_fragscan_init` / `zq_frag_scan`): it consumes a whole buffer per call and
returns the number of bytes used, the fragment size, the prediction hits and
the order-1 table — exactly what the block heuristics in `TJidac.Add` need.
The host reads 64 KiB at a time and copies the consumed run into `fragbuf`
with `Move`; the read size does not affect fragmentation, only throughput.

The scanner reproduces upstream's quirks on purpose: the two multipliers, the
per-fragment reset, the `sz >= maxfrag` cut, and the extra empty fragment when
a file ends exactly on a cut (`fi = vf.Count` runs once, as upstream's
`c = EOF`).

Measured on 200 MB of random data (`taskset -c 7`, min of 3):

| case | before | after |
|---|---|---|
| `add -m0 -t1` | 2.08 s | **1.56 s** |
| `add -m1 -t1` | 2.07 s | **1.54 s** |

The same 200 MB with upstream 7.15 at `-m1 -t1` takes 2.80 s.

`make test` (E1-E6a) and `make test-e6d` (115 CLI checks) stay green; the
archives are bit-identical for `-m0..-m5`, `-fragment 0..19`, empty files,
`-t4` and large inputs.

## 2.1b: hardware SHA-1 with a portable fallback

`add` hashes every fragment (and every block) with SHA-1, so those rounds are
part of the hot path.  The x86 SHA extensions do four rounds per instruction
and expand the message schedule in parallel.

* `src/core/cpu.c` answers `zq_cpu_features()` once: CPUID leaf 7 EBX bit 29
  on x86, `AT_HWCAP` (`HWCAP_SHA1`/`HWCAP_SHA2`) on Linux ARM64,
  `hw.optional.arm.FEAT_SHA1` on macOS, `IsProcessorFeaturePresent` on Windows
  ARM64, and "no features" everywhere else.  Nothing is decided at build time.
* `src/core/sha1.c` keeps the portable, upstream-shaped `sha1_process` and adds
  `sha1_blocks_shani` (the Intel sequence, `__attribute__((target("sha,...")))`).
  `zq_sha1_write` uses it for complete 64-byte blocks when the CPU has SHA-NI
  and the portable loop otherwise; the tail bytes always go through `put()`.
  Leaving the message schedule `w[]` stale after a hardware block is harmless:
  a word is always filled by exactly four `put()` calls, which shift the old
  contents out.

Verification: this host (Xeon E5-2697A v4, Broadwell) has **no** SHA-NI, so
the hardware path is exercised under `qemu-x86_64 -cpu max`:

* `make test-sha1hw` compares both implementations: 361 lengths (0..300 and 60
  random ones up to 1 MiB), 0 differences; it runs under qemu automatically and
  reports "skipped" on a CPU without the extensions.
* The 357 E1 vectors run with SHA-NI give output identical to the oracle
  (`qemu-x86_64 -cpu max build/vectors_e1` vs `build/e1_oracle`).

ARMv8 crypto extensions are detected (`ZQ_CPU_SHA1`) but the ARM64 block
function is not written yet: ARM64 uses the portable path for now, which is
byte-identical and only slower.

## 2.1c: parallel fragmenter (`-turbo`)

Cutting and hashing a large file was still a single-threaded job: one fragment
at a time, and its SHA-1 right after.  `-turbo` splits a file into batches
(64 MiB) and each batch into as many ranges as fragmenter threads, then puts
the ranges back into one chain.

How the chain is rebuilt without changing a byte:

* A range worker starts scanning at *its* range start with a fresh state, so
  the cuts it finds are only correct if that point happens to be a real
  fragment boundary.
* Piece 0 is scanned in the main thread and is correct by construction.
* The stitcher follows the true chain; when it reaches the start of range *k*
  it looks for a fragment of that worker's chain beginning **exactly** at the
  current position.  A match proves the worker's state was the true state from
  that boundary on, so its chain is adopted from there.  Without a match the
  stitcher scans one more fragment serially and tries again.
* Each chain entry carries start, length, `hits`, the order-1 table and the
  fragment's SHA-1, so the main thread only deduplicates and assembles blocks.
* The fragment that is still open at the end of a batch is carried over to the
  front of the next one; at real EOF it is emitted as the last fragment even
  when it is empty (upstream starts that pass with `c = EOF`).

Options: `-turbo [N]` (N fragmenter threads; alone follows `-t`) and
`-noturbo`.  The default follows `-t`, so a single-threaded `add` is unchanged.
Only files of at least 4 MiB are split, and the threshold can be lowered with
`ZPAQII_TURBO_MIN` (a test hook, not a documented option).

200 MB of random data, `taskset -c 0-7`, min of 3:

| case | before | after |
|---|---|---|
| `add -m1 -t1` (serial scanner) | 1.30 s | 1.30 s |
| `add -m1 -t8 -noturbo` | — | 1.16 s |
| `add -m1 -t8` (turbo) | 2.34 s (old `-t8`) | **0.82 s** |
| `add -m0 -t8` (turbo) | 2.25 s (old `-t8`) | **0.92 s** |

Upstream 7.15 on the same 200 MB: 2.80 s (`-m1 -t1`).

Checks: `tests/cli.sh` adds a 3 MB + 1 MB corpus and requires the `-turbo`
archive, the `-noturbo` archive and upstream's to be the same file, and the
extraction to match (118 checks).  Also verified by hand on files that end
exactly on a `maxfrag` cut (the empty trailing fragment) and on a 70 MB file
that crosses a batch boundary.

## 2.3p: the core builds and runs on five targets

The JIT emitted System V x86-64 code but the build only switched it off with
`-DZQ_NOJIT`, so a Windows or ARM build would have compiled it and crashed at
run time.  `zq_internal.h` now decides by target: `ZQ_NOJIT` unless the target
is x86-64 outside Windows and macOS; `jit.c` includes `<sys/mman.h>` only when
the JIT is built.

`make cross` (`tools/cross.sh`) compiles the core for every target with a
compiler here and runs what can be run: Linux x86-64, Linux i386 (`-m32`),
Linux arm64 (under `qemu-aarch64`), and the two mingw builds.  The E1 vectors
are identical to the oracle in all of them; Windows was also run for real, in a
Windows 10 x64 VM and a Windows 7 SP1 x86 VM (357/357 each).

## 2.2a: `-strict` — exit codes that never hide a failure

Decision (2026-11-02): the **default stays 7.15-exact**; the stricter behaviour
is opt-in with `-strict`.  That keeps every differential test against upstream
valid and leaves the port's promise intact.

`-strict` turns a command line that 7.15 answers with the usage text (or with
silence) into a clear error on stderr and exit code **2**:

| command line | 7.15 | ours | ours `-strict` |
|---|---|---|---|
| misplaced word after a switch | usage, 1 | usage, 1 | `misplaced argument`, **2** |
| unknown option | usage, 1 | usage, 1 | `unknown option`, **2** |
| unknown command | usage, 1 | usage, 1 | `unknown command`, **2** |
| command without archive | usage, 1 | usage, 1 | `no archive given`, **2** |
| bad value for a switch | **0** (atoi) | same bytes, 0 | `needs a number`, **2** |

`-strict` is detected before the argument loop, so it works even when it comes
after the offending word; runtime failures keep their codes (upstream's
`error()` exits 1, and a failed extract exits 2).

## 2.2b: `-715` — 7.15 pinned

`-715` forces 7.15 behaviour whatever else is on the line: it clears
`-strict`, disables `-turbo` and is applied after parsing, so argument order
does not matter.  It is the safety switch for the moment the default behaviour
starts to evolve (2.3 and 2.4), and the mode we can always diff against the
oracle.

`tests/cli.sh` checks that `-715 -strict` behaves like 7.15 (usage + exit 1)
and that `-715 -turbo 8` on a corpus that would use the fragmenter produces
upstream's bytes.

## 2.2c: two argument-parsing differences found on the way

Both were real differences from 7.15 (the tests did not cover them):

* `-fragment abc` used to mean fragment size **6** in the port (a defensive
  default) where 7.15 uses `atoi`, i.e. **0**.  The archive differed.  Now it
  is `StrToIntDef(s, 0)`, and `-strict` rejects it instead of guessing.
* `-m 5` (value separated) used to be accepted; 7.15 only accepts the value
  attached (`-m5`).  A bare `-m` now keeps the default method and the next
  word is rejected as misplaced, which is what upstream does.

Both have a check in `tests/cli.sh` (128 checks).

The "compatibility level" report (which program can open this archive) is
deferred: our archive format is still exactly 7.15's, so there is nothing to
report until 2.3/2.4 add extensions.

## 2.3a: extra digests (CRC-32 and XXH64)

`src/core/digest.c` adds two streamed, portable digests next to the SHA-1/SHA-256
already there:

* `zq_crc32` — the IEEE CRC-32 zlib uses (table built once, byte loop);
* `zq_xxh64` — XXH64 with a seed, 32-byte blocks, no unaligned reads and no
  endianness assumptions.

`make test-digest` checks them against vectors generated with `zlib.crc32` and
the reference `xxhash` package, and checks that writing in arbitrary pieces
gives the same answer as one call (71 cases).

## 2.3b: `-hash` stores a digest list inside the archive

`add -hash` appends one more file to the transaction, `VFILE-hash.txt`:

```
zpaqII-hash 1
<xxh64> <crc32> <sha256> <size> <name>
...
```

It is an ordinary file, so plain zpaq 7.15 lists and extracts it (verified: the
7.15 binary lists it as 0644 and extracts it byte for byte), and no format
extension is involved.  The list is built from exactly the files the
transaction adds (`vf`), and the entry goes into both `vf` (data) and `edt`
(index), because the journaling index is written from `edt`.

Verified independently: for a 2-file archive, the `crc32`, `sha256` and
`xxh64` columns match Python's `zlib.crc32`, `hashlib.sha256` and
`xxhash.xxh64` (seed 0).  `tests/cli.sh` adds three checks: 7.15 lists the
file, both programs extract the same tree, and the list has one line per file
(131 checks).

Pending for 2.3: the `check` command that verifies an archive against that
list without writing the files to disk (a stream that hashes instead of
writing, plugged where the extractor opens the output file).
