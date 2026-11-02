# zpaqII

Port de **zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) a **Pascal + C + asm x86-64**,
con compatibilidad **bit-exacta** con el original como criterio de aceptación.

- `docs/PLAN.md` — etapas, criterios de aceptación, riesgos.
- `docs/ARCHITECTURE.md` — mapa de módulos, contrato C↔Pascal, convenciones de port.

## Estado

| etapa | contenido | estado |
|---|---|---|
| E0 | infra, oráculo (upstream compilado), corpus | ✅ |
| E1 | SHA-1, SHA-256, AES-256-CTR, scrypt/stretchKey, random | ✅ idéntico al oráculo (357 casos) |
| E2 | VM ZPAQL + compilador de configs | ✅ idéntico (bytecode y trazas); JIT pendiente |
| E3 | predictor, coder aritmético | pendiente |
| E4 | contenedor, postprocesadores, LZ77/BWT, makeConfig | pendiente |
| E5 | archiver JIDAC en Pascal | pendiente |
| E6 | matriz bit-exacta, fuzzing, performance | pendiente |

## Build

```
make            # core C + tests
make test       # diferencial contra el upstream de referencia
make DEBUG=1    # ASan/UBSan
make NOJIT=1    # sin JIT (sólo intérprete ZPAQL)
```

El upstream de referencia se clona en `$UPSTREAM`
(por defecto `/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq`) y se compila con `make oracle`.

## Licencia

Unlicense, igual que upstream. `divsufsort` conserva su licencia MIT.
