# zpaqII — arquitectura y convenciones del port

## 1. Mapa de módulos

Referencia upstream → módulo del port. Los rangos de línea son del commit `9ab539f`.

| upstream | líneas | port | lenguaje |
|---|---|---|---|
| `libzpaq.cpp`: allocx, SHA1, SHA256, AES, stretchKey, random | 60–706 | `core/zq_util.c`, `core/sha1.c`, `core/sha256.c`, `core/aes.c` | C |
| `Component`, `StateTable` | 707–861 | `core/state.c`, `core/component.c` | C |
| `ZPAQL` (VM, flags, headers) | 862–1268 | `core/zpaql.c` | C |
| `ZPAQL::assemble` (JIT) | 3033–3812 | `core/jit.c` + `asm/zpaql_jit_x86_64.S` | C + asm |
| `Predictor` | 1269–2089 | `core/predictor.c` | C |
| `Decoder` | 2090–2182 | `core/coder.c` | C |
| `PostProcessor` | 2183–2242 | `core/postproc.c` | C |
| `Decompresser` | 2243–2390 | `core/decompresser.c` | C |
| `Encoder` | 2391–2448 | `core/coder.c` | C |
| `Compiler` (texto→bytecode) | 2449–2771 | `core/zpaql_asm.c` | C |
| `Compressor` | 2772–2833 | `core/compressor.c` | C |
| `MemoryReader`, `compress()` | 2834–3032 | `core/buffer.c`, `core/api.c` | C |
| `Predictor::assemble_p` (JIT predictor) | 3813–4657 | `core/jit.c` | C + asm |
| `divsufsort` / `divbwt` | 4658–6435 | `core/divsufsort.c` | C (MIT) |
| LZ77 / BWT encoder (`makeConfig` helpers) | 6436–6886 | `core/lz77.c` | C |
| `makeConfig`, `method` parsing, `probe` | 6887–7733 | `core/config.c`, `core/method.c` | C |
| `zpaq.cpp` entero (archiver JIDAC) | 1–3737 | `host/*.pas` | Pascal |

El **árbol no se portará línea a línea en el mismo orden**: cada módulo se traduce
con su test contra el oráculo antes de pasar al siguiente.

### Módulos del host Pascal (`host/`)

| unidad | responsabilidad | origen upstream |
|---|---|---|
| `zqfile.pas` | `File` layer: fopen/fread/fseek, fechas, atributos, makepath, truncate | `zpaq.cpp` 376–593 |
| `zqtime.pas` | `mtime`, `decimal_time`, `unix_time`, `dateToString`, `attrToString` | 292–375 |
| `zqarchive.pas` | `ArchiveBase`, `InputArchive`, `OutputArchive`, multi-parte | 594–841 |
| `zqscan.pas` | `scandir`, `ispath`, `isselected`, `rename`, path join | 909–942, 1665–1811 |
| `zqjidac.pas` | estructuras `HT/DT/Block/VER`, `usage`, `doCommand` | 943–1351 |
| `zqreadarc.pas` | `read_archive`: parseo de bloques, segmentos y comentarios JIDAC | 1352–1664 |
| `zqadd.pas` | `add`: fragmentación, dedup, journaling, `CompressJob`, hilos | 1812–2711 |
| `zqextract.pas` | `extract`, `ExtractJob`, `decompressThread`, `equal` | 2712–3506 |
| `zqlist.pas` | `list` | 3507–3698 |
| `zqmain.pas` | `main`, WTOU/UTOW, printUTF8 | 236–291, 3699–3737 |
| `zqthreads.pas` | hilos, mutex, semáforo, cola | 154–235 |

## 2. Contrato C↔Pascal

Cabecera pública única: `include/zq.h`. Nada más cruza el límite.

```c
/* Flujos: callbacks en estilo C, `self` opaco del host. */
typedef struct zq_reader {
  int     (*get)(void *self);                        /* 0..255, -1 = EOF (obligatorio) */
  int     (*read)(void *self, char *buf, int n);     /* opcional; NULL = bucle con get() */
  void    *self;
} zq_reader;

typedef struct zq_writer {
  void    (*put)(void *self, int c);                 /* obligatorio */
  void    (*write)(void *self, const char *buf, int n); /* opcional */
  void    *self;
} zq_writer;

/* Buffer en memoria (equivalente a libzpaq::StringBuffer / MemoryReader). */
typedef struct zq_buf zq_buf;
zq_buf* zq_buf_new(void);
void    zq_buf_free(zq_buf*);
void    zq_buf_put(zq_buf*, int c);
void    zq_buf_write(zq_buf*, const char *p, int n);
void    zq_buf_clear(zq_buf*);
size_t  zq_buf_size(zq_buf*);
void    zq_buf_seek(zq_buf*, size_t);   /* posición de lectura para MemoryReader */
int     zq_buf_get(zq_buf*);
const char *zq_buf_data(zq_buf*);
```

SHA/AES/random: células independientes, con init/put/result como upstream.

Compresión/descompresión de bloques (lo único que usa el archiver):

```c
void zq_compress_block(zq_buf *in, zq_writer *out, const char *method,
                       const char *filename, const char *comment, int dosha1);

typedef struct zq_decompresser zq_decompresser;
zq_decompresser* zq_d_new(void);
void zq_d_free(zq_decompresser*);
void zq_d_set_input(zq_decompresser*, zq_reader*);
int  zq_d_find_block(zq_decompresser*, double *mem);     /* 1 = hay bloque */
int  zq_d_compressed(zq_decompresser*);
int  zq_d_decompress(zq_decompresser*, int n);           /* n bytes, -1 = todo */
int  zq_d_read_segment_end(zq_decompresser*, char *sha1out21, int64_t *size, int verify);
void zq_d_hcomp(zq_decompresser*, zq_writer*);
void zq_d_pcomp(zq_decompresser*, zq_writer*);
```

## 3. Convenciones de traducción C++ → C

Se traduce **literal**: mismos nombres, mismo orden de operaciones, mismos tipos con
ancho fijo. No se "mejora" nada mientras no haya tests que lo respalden.

| C++ (upstream) | C (port) |
|---|---|
| `class X { ... }` | `typedef struct zq_X { ... } zq_X;` + `zq_x_method(...)` |
| `Array<T> a(n)` | `zq_u8arr` / `zq_u32arr` (alineados a 64 B, calloc) |
| `Reader`/`Writer` virtuales | struct de callbacks (§2) |
| `std::string` | no existe en core; buffers + longitud |
| `std::vector<T>` | `zq_vec_*` (crece por duplicación) |
| `throw`/`error()` | `zq_error(msg)` (idéntico al upstream: mensaje a stderr + `exit(1)`) |
| `U8/U16/U32/U64` | `uint8_t/uint16_t/uint32_t/uint64_t` |
| `int` de 32 bits con wrap | `int32_t`/`uint32_t` explícitos; shifts enmascarados a 31/63 |
| `char` (con signo en x86) | `int8_t`/`uint8_t` explícito en cada uso |

Reglas duras:

1. **Sin UB no intencional**: todo shift con variable se enmascara igual que upstream
   (`&31` para 32 bits, `&63` para 64); conversiones `double→int64` documentadas.
2. **Alineación**: los buffers de modelos se alinean a 64 B (upstream lo hace así y
   los kernels asm lo requieren).
3. **Endianness**: el formato es little-endian; el código usa memcpy/bytes, nunca
   punteros a `U32` sobre un stream.
4. **Errores**: mismos strings y mismo orden de validación que upstream (los tests
   comparan stderr y exit code).
5. **Orden de evaluación**: si upstream depende del orden (p. ej. `a=b++;`), se
   desdobla en sentencias separadas, nunca se deja al compilador.

## 4. Build

```
make            # release: core C (-O3) + host Pascal + asm
make DEBUG=1    # -O0 -g -fsanitize=address,undefined (core) + -Cr -Co (Pascal)
make NOJIT=1    # sin JIT, intérprete ZPAQL solamente (portabilidad)
make test       # unit + diferencial contra oráculo
make matrix     # matriz completa bit-exacta
make oracle     # compila upstream en /mnt/IA_LAB/agentes/zpaqII/upstream-zpaq
```

El host se compila con `fpc -Mobjfpc -O2 -Sh -vew`; el core con
`gcc -std=c11 -O3 -march=x86-64-v2 -Wall -Wextra -Wshadow`.

## 5. Tests

- `tests/unit/`: vectores NIST (SHA/AES), tablas de estado, opcodes ZPAQL.
- `tests/oracle/`: harness C++ compilado contra upstream que vuelca trazas
  (`p` por bit, `H[]`, bytecode, salida del coder) en formato estable.
- `tests/diff/`: comparadores (byte a byte y traza) usados por `make test`.
- `tests/matrix.sh`: matriz de archivos reales (niveles × métodos × opciones).
- `tests/corpus/`: archivos sintéticos generados por `tests/corpus.sh` con mtime fijo.

Nada de tests "a ojo": todo criterio es binario (cmp / hash / exit code).
