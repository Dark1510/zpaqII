# zpaqII — what it is, where it stands, what is left

zpaqII is **a concept: the spiritual successor to zpaq**.  It starts as a port of
**zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) to **Pascal + C + x86-64
assembly**, and bit-exact compatibility with the original is the acceptance
criterion for everything that exists today.  From there it moves the archiver
forward on its own while keeping every archive it writes readable by plain zpaq
7.15.

The VM engine is called **ZPAQL(II)**: same instruction set, same semantics,
new engine.  The name is documentation only; the bytecode and the archives are
7.15's.

## The rule that governs every change

1. **Default behaviour is 7.15-exact**: archives, extracted files, exit codes,
   console output and speed.  Where 7.15 has a quirk, the quirk is reproduced,
   not fixed.
2. **Improvements are opt-in** (`-strict`, `-turbo`, `-hash`), and
   `-715` puts everything back to 7.15 regardless of argument order.
3. **Anything we write, plain zpaq 7.15 can read.**  The compression methods
   are 7.15's own `-m0`…`-m5` (and their `x`/`s`/`i` forms); there are no
   external codecs (`-ma`), see `docs/DECISIONS.md` ADR-006.
4. **The oracle is the upstream binary**, plus harnesses that instrument it —
   never the prose specification.

## Where it stands

Phase 1 (the port) is complete and bit-exact.  Phase 2 (the successor) is
partly done.

| area | state |
|---|---|
| E0–E6: SHA-1/SHA-256/AES/scrypt, ZPAQL VM, predictor and arithmetic coder, container and postprocessors, JIDAC archiver, differential matrix and fuzzing | ✅ done, bit-exact against the oracle |
| ZPAQL JIT and predictor JIT (x86-64 Linux) | ✅ done |
| 2.1 parallel fragmenter (`-turbo`) and hardware SHA-1 (SHA-NI) | ✅ done; ARMv8 SHA-1 still uses the portable path |
| 2.2 exit-code hygiene (`-strict`) and `-715` | ✅ done |
| 2.3 configurable integrity (`-hash`, `check`, `-savelist`, `-checklist`) | ✅ done |
| 2.4 external codecs (`-ma`) | ❌ discarded (ADR-006); the LZ4 work was removed from the tree |
| ZPAQL(II) phase A: new interpreter loop | ✅ done (2.2× on `-m1`/`-m2` in interpreter-only builds) |
| ZPAQL(II) phase B: W^X, batched runs, native fast paths for `lazy2`/`lzpre`/`e8e9` | ✅ done (−28 % CPU extracting `-m1`/`-m2`) |
| ZPAQL(II) phase B: native inverse BWT, superinstructions, compile cache | 🚧 **next** |

### Platforms

| target | state |
|---|---|
| Linux x86-64 | ✅ full, including the JIT |
| Linux arm64 | ✅ full (interpreter; no JIT) |
| Linux x86 (i386) | ✅ cross-build, byte-identical output |
| Windows x64 (7 and newer) | ✅ built with FPC + mingw, tested in Win7 and Win10 VMs |
| Windows x86 (7 and newer) | ⏳ needs a FreePascal toolchain for 32-bit Windows |
| macOS arm64 | ✅ builds in CI; the CLI suite has not been run there yet |

CI runs six jobs (linux-x86_64, linux-arm64, cross, cross-i386, windows-wine,
macos-arm64) and all are green on the current head.

### What you can do with it today

`add`, `extract`, `list`, `test`, `check`; `-key`, `-index`, `-to`, `-until`,
`-fragment`, `-tN`, `-summary`, `-noattributes`, `-all`, `-repack`;
`-strict`, `-715`, `-turbo [N]`, `-hash`, `-savelist`, `-checklist`.
A `-hash` archive carries its digest list (`VFILE-hash.txt`) as an ordinary
file, so plain zpaq lists it like any other and zpaqII can verify it without
writing anything to disk.

## What is left

In the order it will be done.

### 1. ZPAQL(II) phase B: the engine

Done: W^X for both JITs, batched runs (one call into the JIT per 4 KB),
native fast paths for the `lazy2`, `lzpre` and `e8e9` postprocessors
(`docs/ZPAQL.md`).  Left:

* **Native inverse BWT** (`bwtrle`, already recognised): `-m3` on text.
* **Superinstructions** for common pairs in the JIT.
* **Compile cache** keyed by the config, for archives with many blocks.

### 2. Platform loose ends

* ARMv8 SHA-1 block function (detection exists, the portable path is used).
* Windows **x86** host build (needs FPC on Windows).
* Run the CLI suite on macOS.
* Compare bit-exactly against an upstream **built on Windows**; today only
  functional compatibility in both directions is verified.

### 3. 2.5: optional platform extras

Only on concrete demand: sector-level imaging, Windows ACLs/ADS/long paths,
ZFS integration, an authenticated outer container, remote transports.
Details and the reasons each one is optional: `docs/ROADMAP.md` §5.

### Parked, deliberately not worked on

The forks' own open issues (`docs/LIMITATIONS.md` §2.3): a format without
SHA-1, fast listing without history, new CM models, the 32-bit byte
differences, experimental codecs, packaging.

## How to verify it yourself

```
make test          # differential tests against the reference upstream
make matrix        # full bit-exact matrix (add/extract, both directions)
make test-e6d      # CLI option parity with upstream (tests/cli.sh)
make test-zpaqldif # new interpreter loop and batched JIT vs the one-step reference
make test-fastpath # native fast paths vs the one-step reference and real blocks
make cross         # Linux x64/i386/arm64 + mingw x64/x86
```

Every one of these compares against the oracle; none of them is a self-test
that only proves the code agrees with itself.

## Where to read more

| document | what it holds |
|---|---|
| `docs/PLAN.md` | staged log: what was done, with measurements |
| `docs/ROADMAP.md` | phase 2 backlog, order and acceptance criteria |
| `docs/ARCHITECTURE.md` | module map, C↔Pascal contract, conventions |
| `docs/DECISIONS.md` | ADRs (oracle, language split, error handling, threads) |
| `docs/DEVIATIONS.md` | deliberate differences, if any |
| `docs/ZPAQL.md` | ZPAQL(II): the engine, the contract, measurements |
| `docs/JIT.md` | the two JITs |
| `docs/PORTABILITY.md` | what is portable and what blocks each target |
| `docs/LIMITATIONS.md` | what zpaq 7.15 and the forks cannot do |
| [wiki](https://github.com/YadeWira/zpaqII/wiki) | the same, for readers |
