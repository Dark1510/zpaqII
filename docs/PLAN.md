# zpaqII — plan del port

Port de **zpaq 7.15** (github.com/zpaq/zpaq, Unlicense) a **Pascal (host) + C (core) + asm x86-64 (rutas calientes)**.

Referencia congelada: commit `9ab539f` ("Fixes incorrect JIT assembly of \"a+= 128\"...").
Copia de trabajo: `/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq` (binario oráculo incluido).

---

## 1. Objetivo y criterio de aceptación

**Objetivo:** compatibilidad **bit-exacta total** con zpaq 7.15.

Se considera cumplido cuando se verifican las cuatro propiedades, para cada caso de la matriz:

1. **Extract:** un `.zpaq` producido por upstream se extrae con zpaqII byte a byte igual.
2. **Add:** con la misma entrada, mismas fechas/atributos, mismas opciones y mismo `-t`,
   zpaqII produce un `.zpaq` **idéntico byte a byte** al de upstream.
3. **Cross:** cada dirección anterior se prueba también al revés (zpaqII add → upstream extract,
   upstream add → zpaqII extract).
4. **CLI:** mismos comandos/opciones/comportamiento observable (mensajes, códigos de salida,
   `list`, `-until`, `-index`, multi-parte, cifrado).

Matriz mínima: `-m0..-m5`, `-method x1..x14,i1..i6,s1..s7` (subconjunto representativo),
con y sin `-t`, dedup entre versiones, `-until`, `-key`, multi-parte, nombres UTF-8,
archivos vacíos, binarios, texto, ya comprimidos, y > 1 bloque.

**Nota sobre hilos:** `-t4` y `-t1` **no** producen los mismos bytes en `-m4/-m5`
(verificado: mismo tamaño, contenido distinto). El criterio bit-exacto se exige
**para el mismo número de hilos que upstream**. Reproducir el orden de escritura
multihilo es parte del port (ver §5, riesgo R3).

---

## 2. Arquitectura

```
                 ┌──────────────────────────────────────────────┐
   CLI / archiver│  Pascal (FPC 3.2.2)  host/                   │
   journaling    │  cli, scan, dedup, index, add/list/extract,  │
   I/O, hilos    │  hilos, cifrado de archivo, multi-parte      │
                 └───────────────┬──────────────────────────────┘
                                 │ C ABI (callbacks Reader/Writer)
                 ┌───────────────┴──────────────────────────────┐
   códec         │  C (gcc 14, -std=c11)  core/                │
   VM ZPAQL      │  sha1/256, aes, state table, zpaql (asm+VM), │
   predictor     │  predictor, coder aritmético, postproc,      │
   coder         │  config/method, divsufsort, JIT x86-64       │
                 └───────────────┬──────────────────────────────┘
                                 │
                 ┌───────────────┴──────────────────────────────┐
   rutas         │  asm/  (GAS, Intel syntax)                   │
   calientes     │  coder, update de modelos, memcpy/bit ops,   │
                 │  plantillas del JIT                          │
                 └──────────────────────────────────────────────┘
```

Regla de oro de la separación: **el core C no conoce archivos ni hilos**; sólo flujos
(`zq_reader`/`zq_writer`), buffers y parámetros. El host Pascal no conoce el formato
comprimido; sólo bloques, segmentos y la API del core.

### Contrato C (equivalente a libzpaq.h)

La superficie que el archiver necesita de libzpaq es chica (medida con grep sobre zpaq.cpp):

| API | uso en zpaq.cpp |
|---|---|
| `Reader` / `Writer` | I/O de archivo y buffers |
| `StringBuffer` | acumular fragmentos antes de comprimir |
| `SHA1` | checksums de bloque/segmento y dedup |
| `AES_CTR`, `stretchKey`, `random` | cifrado |
| `Decompresser` (`findBlock`, `setInput`, `decompress`, `readSegmentEnd`, `getMemory`, `hcomp`, `pcomp`) | extraer/listar |
| `compressBlock(StringBuffer*, Writer*, method, filename, comment, dosha1)` | comprimir un bloque |
| `Array` | buffers alineados a 64 bytes |

Todo lo demás (Predictor, ZPAQL, Compiler, divsufsort) es interno del core.

---

## 3. Etapas

Cada etapa deja el árbol compilando y con tests verdes. El orden está pensado para
que el oráculo (upstream) valide lo antes posible.

### E0 — Infraestructura y oráculo
- Layout del repo, `Makefile`, `docs/` (este plan, ARCHITECTURE, ADRs).
- `tools/oracle/`: build del upstream + harness instrumentado que vuelca estados
  intermedios (bytecode compilado, tablas de estado, predicciones p, H[], salida del
  coder) como texto/binario para comparar contra el core C.
- Generador de corpus reproducible (`tests/corpus.sh`): ficheros sintéticos con mtime
  fijo + muestras de `/mnt/OSR_D3/fileFormatSamples` (copiadas, nunca in situ).
- `tests/run.sh`: matriz de comparación (hash de archivo, `cmp`, exit codes).
- **Aceptación:** `make oracle && make corpus && make test` corre y falla sólo porque
  el port aún no existe.

### E1 — Primitivas deterministas (C)
- `sha1.c`, `sha256.c`, `aes.c` (+ `stretchkey`), `random.c`, `array.c` (buffers 64B,
  cero-inicializado), manejo de error compatible (`zq_error` → exit 1).
- **Aceptación:** vectores conocidos (NIST/FIPS) + comparación byte a byte con el
  harness del oráculo para claves/tamaños aleatorios y `stretchKey` (100k iteraciones
  en upstream → PBKDF2-SHA256 propio de zpaq).

### E2 — VM ZPAQL (C + asm)
- `zpaql_asm.c`: compilador de texto ZPAQL → bytecode (`OPCODES`/table de upstream,
  `Compiler::compile`), incluido `pcomp` y comentarios.
- `zpaql_vm.c`: intérprete `run0`/`run` con la semántica exacta (A/B/C/D/R/H/X,
  saltos, `HASH`, `OUT`, `HALT`, wrap de 32 bits, `A<<=X` con `X&31`, etc.).
- `asm/zpaql_jit_x86_64.S` + `jit.c`: traducción a código máquina por bloque
  (equivalente a `ZPAQL::assemble`), con fallback al intérprete (`NOJIT`).
- **Aceptación:** para todos los configs de `-m0..-m5` y `-method ...`, el bytecode
  compilado es idéntico al del oráculo; ejecutar HCOMP/PCOMP y comparar `H[]`,
  salidas y tiempos. Diferencial: intérprete ≡ JIT en 10^6 pasos aleatorios.

### E3 — Predictor y coder (C + asm)
- `state.c` (StateTable), `component.c` (CONS/CM/ICM/MATCH/AVG/MIX2/MIX/ISSE/SSE),
  `predictor.c` (init/update/predict, hmap4, c8, nibble boundaries, `MATCH` buffer).
- `coder.c`: `Decoder` y `Encoder` (range coder carryless), `decode(0)` = EOS.
- `asm/coder_x86_64.S` y `asm/model_x86_64.S`: inner loops (ver §4).
- **Aceptación:** traza por byte de `p` y de `c0` idéntica al oráculo en los 5 niveles;
  decodificar bloques sueltos del oráculo bit a bit.

### E4 — Contenedor y API de alto nivel (C)
- `decompresser.c` (findBlock, headers, HCOMP/PCOMP, verificación SHA-1, memoria),
  `postproc.c` (lazy2, E8E9, BWT!) — el BWT del nivel 2 exige prestar atención al
  modelo de contexto de bytes (`bwt` en pcomp).
- `compressor.c` (`compressBlock`), `config.c` (`makeConfig` textual de todos los
  niveles/métodos), `method.c` (parseo `x/i/s/0` + args), `divsufsort.c` (port del
  embebido, C puro, sin cambios de semántica).
- **Aceptación:** round-trip de los 5 niveles + `compressBlock` bit-exacto contra
  upstream para el corpus; verificación SHA-1 de bloques corruptos debe fallar igual.

### E5 — Archiver (Pascal)
- `fio.pas` (file layer, fechas/atributos win/unix), `archive.pas` (Input/OutputArchive,
  multi-parte, seeks), `scan.pas` (scandir, symlinks, exclusiones `-not`, `-only`),
  `readarc.pas` (lectura del índice JIDAC: `jDC\x01`, bloques de fragmentos),
  `add.pas` (fragmentación, dedup SHA-1, journaling, versiones, `-until`),
  `extract.pas`, `list.pas`, `index.pas`, `cli.pas`, `threads.pas` (pool + semáforos
  con la misma política de orden que upstream).
- **Aceptación:** matriz completa de E6 sobre comandos y opciones, incluidos
  `-key`, multi-parte, `-until`, `-fragment`, `-index`, y errores/exit codes.

### E6 — Verificación total y performance
- `tests/matrix.sh`: matriz cruzada de 2 direcciones × niveles × métodos × opciones.
- Fuzzing diferencial del decoder (archivos truncados/corruptos: mismo mensaje de
  error y mismo exit code que upstream, sin crashes ni leaks).
- Benchmarks: ratio idéntico (garantizado por bit-exactitud) y velocidad objetivo
  ≥ 90 % de upstream en `-m1/-m2`; análisis con `perf` de los loops asm.
- Sanitizers: `-fsanitize=address,undefined` en core C; `-Cr -Co` en Pascal para CI
  (sin afectar el binario release).
- **Aceptación:** matriz 100 % verde, sin warnings con `-Wall -Wextra`, valgrind/ASan
  limpio en decode de corpus.

---

## 4. Dónde va el asm (y por qué)

| Rutina | Motivo | Implementación |
|---|---|---|
| `Decoder::decode` / `Encoder::encode` (byte-level) | ~1 op por byte comprimido, dominio del tiempo | asm inline en `coder.c` + fallback C |
| Update de ICM/ISSE/MIX/SSE/CM (`p[i] += ...`, `dt[]` lookups) | ~8 modelos por bit | asm de bloque `asm/model_x86_64.S` |
| `ZPAQL` HCOMP/PCOMP | se ejecuta por byte de entrada | JIT que emite código (equivalente a `ZPAQL::assemble`) |
| `memcpy`/`memset` de buffers de 64B alineados, `bswap`, `popcnt/lzcnt` | accesos calientes | asm con detección de CPU (`cpuid`) |
| Opcional: SHA-1 con SHA-NI | dedup de fragmentos | asm condicional |

Regla: **el asm nunca cambia semántica**. Cada kernel tiene test diferencial contra la
versión C pura (misma función, compilada sin asm), y el JIT contra el intérprete.
`make NOJIT=1` debe producir el mismo output (sólo más lento).

---

## 5. Riesgos y mitigaciones

- **R1 — UB del C++ original.** Upstream hace shifts con signo, `char` con signo,
  aliasing y aritmética de 32 bits con wrap. El port debe fijar esos comportamientos
  explícitamente (`uint32_t`, `U8`, shifts enmascarados). Mitigación: tests
  diferenciales por byte y `-fwrapv`-equivalente documentado en C.
- **R2 — JIT.** El JIT es una optimización, no semántica; se puede shippear primero el
  intérprete. Pero upstream lo usa por default y el bit-exacto no depende de él.
  Mitigación: diferencial JIT vs intérprete en E2 antes de habilitarlo.
- **R3 — Multihilo.** El orden de escritura de bloques en `-t>1` afecta los bytes.
  Mitigación: portar la política de `CompressJob`/`writeThread`/`Semaphore` tal cual;
  verificar `-t1` primero y luego `-t2/-t4/-t8` con el mismo input.
- **R4 — Memoria.** Los niveles altos usan cientos de MB por hilo (upstream reserva y
  toca páginas). Mitigación: misma estrategia de reserva/zeroing, tests con `ulimit`.
- **R5 — Formatos de fecha/atributo.** Windows vs unix (`decimal_time`, atributos
  MSDOS). Mitigación: fijar `-Dunix` como upstream en Linux; la tabla se porta tal cual.
- **R6 — Corpus grande.** `archive` del corpus pesa 59 GB. Mitigación: copiar muestras
  chicas filtradas por tamaño a `/mnt/IA_LAB/agentes/zpaqII/corpus`.
- **R7 — Alcance.** 11 k líneas C++ no se portan de una sentada sin errores.
  Mitigación: etapas con aceptación binaria, y el oráculo como red de seguridad en
  cada commit.

---

## 6. Convenciones del repo

- `core/` sólo C11 + asm; sin dependencias externas; sin I/O de archivos.
- `host/` sólo Pascal; sin lógica de compresión.
- Nombres: `zq_*` para el core, `T*` para clases Pascal.
- Un commit por módulo con su test; nunca dejar el árbol sin compilar.
- Licencia: Unlicense (igual que upstream); `divsufsort` mantiene su MIT.
