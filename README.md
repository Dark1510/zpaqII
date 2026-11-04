# zpaqII

[![ci](https://github.com/YadeWira/zpaqII/actions/workflows/ci.yml/badge.svg)](https://github.com/YadeWira/zpaqII/actions/workflows/ci.yml)

**zpaqII is a concept: the spiritual successor to zpaq.** It starts as a port of
**zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) to **Pascal + C + x86-64
assembly** — bit-exact compatibility with the original being the acceptance
criterion for everything that exists today — and then carries the archiver
forward on its own, keeping the archives readable by plain zpaq 7.15.

## Origin

The idea for zpaqII came from this encode.su forum thread:

<https://encode.su/threads/4387-zpaq2>

## Documentation

- [Wiki](https://github.com/YadeWira/zpaqII/wiki) — what it is, extensions, platforms, roadmap
- `docs/ZPAQL.md` — **ZPAQL(II)**, the VM engine: same instruction set, new engine
- `docs/PLAN.md` — stages, acceptance criteria, risks.
- `docs/ARCHITECTURE.md` — module map, C↔Pascal contract, porting conventions.
- `docs/DECISIONS.md` — ADRs (oracle, language split, error handling, threads).

## Status

| Component | Status |
|---|---|
| CI | [![ci](https://github.com/YadeWira/zpaqII/actions/workflows/ci.yml/badge.svg)](https://github.com/YadeWira/zpaqII/actions/workflows/ci.yml) |
| Latest release | not published yet |
| C core (`src/core`) | ✅ bit-exact with 7.15 |
| Pascal host (`host/`) | ✅ add, extract, list, test, check |
| ZPAQL VM, interpreter | ✅ ZPAQL(II) phase A (`docs/ZPAQL.md`) |
| ZPAQL VM, JIT | ✅ x86-64 Linux only |
| Predictor JIT | ✅ x86-64 Linux only |
| Hardware SHA-1 | ✅ x86 SHA-NI; ARMv8 pending |
| Turbo fragmenter (`-turbo`) | ✅ |
| Digests (`-hash`, `check`) | ✅ |
| LZ4 codec | 🚧 codec and ZPAQL decoder done; block framing pending (2.4b) |
| Linux x86-64 / arm64 | ✅ CI |
| Windows x64 | ✅ tested in Win7 and Win10 VMs |
| Windows x86 host | ⏳ needs FPC on Windows |
| macOS arm64 | ✅ CI builds; CLI tests pending |

Stages:

| stage | content | state |
|---|---|---|
| E0 | infrastructure, oracle (upstream build), corpus | ✅ |
| E1 | SHA-1, SHA-256, AES-256-CTR, scrypt/stretchKey, random | ✅ identical to oracle (357 cases) |
| E2 | ZPAQL VM + config compiler | ✅ identical (bytecode and traces); JIT ✅ |
| E3 | predictor, arithmetic coder | ✅ identical (13 models, 4608 byte traces, encoder bytes); asm kernels pending |
| E4 | container, postprocessors, LZ77/BWT, makeConfig | ✅ 38 archives extracted + 51 blocks compressed bit-exactly |
| E5 | JIDAC archiver in Pascal | ✅ streaming and journaling add/extract bit-identical (levels 0-5, updates, deletions, -until, -fragment, -to, -only/-not, -key, -index, multi-part, -summary, -test, -noattributes, -all, -repack); parallel -tN ✅ |
| E6 | bit-exact matrix, fuzzing, performance | fuzzing ✅; matrix ✅; ZPAQL JIT ✅; predictor JIT ✅ (`docs/JIT.md`); performance at or above upstream on -m1..-m5, stored blocks and extraction |

## Extensions beyond 7.15

The archive format is still exactly 7.15's, so plain zpaq can read everything
zpaqII writes.  Phase 2 adds behaviour, never bytes:

| option | what it does |
|---|---|
| `-turbo [N]` / `-noturbo` | cut and hash large files in N threads (default: as `-t`). The fragments, and the archive, are the ones the serial loop produces. |
| `-strict` | command-line errors (unknown option or command, misplaced word, missing archive, bad value) exit 2 with a message instead of 7.15's usage-and-1 or silent 0. |
| `-715` | pin 7.15 behaviour and bytes: clears `-strict`, disables `-turbo`; whatever the argument order. |
| `-hash` | on `add`, store a digest list (`VFILE-hash.txt`: XXH64, CRC-32, SHA-256, size, name per file) as an ordinary file of the transaction, so plain zpaq still reads the archive. |
| `check` | command: verify every file of the archive against that list without writing anything to disk (the extractor's output stream hashes instead). Exit 0 ok, 1 mismatch, 2 no list. `-savelist F` exports the list, `-checklist F` verifies against an external one. |

## Build

```
make            # C core + tests
make test       # differential tests against the reference upstream
make matrix     # full bit-exact matrix (add/extract, both directions)
make test-e6c   # JIT vs interpreter differential test
make test-e6d   # CLI option parity with upstream (tests/cli.sh)
make DEBUG=1    # ASan/UBSan
make NOJIT=1    # no JIT (ZPAQL interpreter only)
make oracle     # clone and build the reference upstream
```

The reference upstream is cloned into `$UPSTREAM`
(default `/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq`).

## License

Unlicense, same as upstream. `divsufsort` keeps its MIT license.
