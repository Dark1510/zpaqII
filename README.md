# zpaqII

Port of **zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) to **Pascal + C + x86-64
assembly**, with **bit-exact** compatibility with the original as the acceptance
criterion.

- `docs/PLAN.md` — stages, acceptance criteria, risks.
- `docs/ARCHITECTURE.md` — module map, C↔Pascal contract, porting conventions.
- `docs/DECISIONS.md` — ADRs (oracle, language split, error handling, threads).

## Status

| stage | content | state |
|---|---|---|
| E0 | infrastructure, oracle (upstream build), corpus | ✅ |
| E1 | SHA-1, SHA-256, AES-256-CTR, scrypt/stretchKey, random | ✅ identical to oracle (357 cases) |
| E2 | ZPAQL VM + config compiler | ✅ identical (bytecode and traces); JIT pending |
| E3 | predictor, arithmetic coder | ✅ identical (13 models, 4608 byte traces, encoder bytes); asm kernels pending |
| E4 | container, postprocessors, LZ77/BWT, makeConfig | ✅ 38 archives extracted + 51 blocks compressed bit-exactly |
| E5 | JIDAC archiver in Pascal | streaming add/extract ✅ (23 archives bit-identical); JIDAC read/list/extract ✅ (all levels, multi-version, -until); journaling add pending |
| E6 | bit-exact matrix, fuzzing, performance | pending |

## Build

```
make            # C core + tests
make test       # differential tests against the reference upstream
make DEBUG=1    # ASan/UBSan
make NOJIT=1    # no JIT (ZPAQL interpreter only)
make oracle     # clone and build the reference upstream
```

The reference upstream is cloned into `$UPSTREAM`
(default `/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq`).

## License

Unlicense, same as upstream. `divsufsort` keeps its MIT license.
