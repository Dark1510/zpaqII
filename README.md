# zpaqII

**zpaqII is the spiritual successor to zpaq.** It starts as a port of
**zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) to **Pascal + C + x86-64
assembly** — bit-exact compatibility with the original being the acceptance
criterion for everything that exists today — and then carries the archiver
forward on its own, keeping the archives readable by plain zpaq 7.15.

## Origin

The idea for zpaqII came from this encode.su forum thread:

<https://encode.su/threads/4387-zpaq2>

## Documentation

- `docs/PLAN.md` — stages, acceptance criteria, risks.
- `docs/ARCHITECTURE.md` — module map, C↔Pascal contract, porting conventions.
- `docs/DECISIONS.md` — ADRs (oracle, language split, error handling, threads).

## Status

| stage | content | state |
|---|---|---|
| E0 | infrastructure, oracle (upstream build), corpus | ✅ |
| E1 | SHA-1, SHA-256, AES-256-CTR, scrypt/stretchKey, random | ✅ identical to oracle (357 cases) |
| E2 | ZPAQL VM + config compiler | ✅ identical (bytecode and traces); JIT ✅ |
| E3 | predictor, arithmetic coder | ✅ identical (13 models, 4608 byte traces, encoder bytes); asm kernels pending |
| E4 | container, postprocessors, LZ77/BWT, makeConfig | ✅ 38 archives extracted + 51 blocks compressed bit-exactly |
| E5 | JIDAC archiver in Pascal | ✅ streaming and journaling add/extract bit-identical (levels 0-5, updates, deletions, -until, -fragment, -to, -only/-not, -key, -index, multi-part, -summary, -test, -noattributes, -all, -repack); parallel -tN ✅ |
| E6 | bit-exact matrix, fuzzing, performance | fuzzing ✅; matrix ✅; ZPAQL JIT ✅; predictor JIT ✅ (`docs/JIT.md`); performance at or above upstream on -m1..-m5, stored blocks and extraction |

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
