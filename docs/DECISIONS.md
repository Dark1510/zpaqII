# Decisiones (ADR)

## ADR-001 — El oráculo es binario, no la documentación

**Decisión:** la referencia es zpaq 7.15 compilado (commit `9ab539f`) y los criterios
son binarios: `cmp` de archivos, diff de trazas, exit codes. No se usa la prosa del
spec como fuente de verdad cuando contradice al binario.

**Consecuencia:** cada etapa agrega un harness que instrumenta el upstream
(`tools/oracle/`) y un test diferencial. Un módulo se considera terminado sólo cuando
el diff es vacío. Es lo que permitió detectar, por ejemplo, que `-t1` y `-t4` no
producen los mismos bytes en `-m4/-m5`.

## ADR-002 — Reparto Pascal / C / asm

**Decisión:** Pascal (FPC) para CLI, archiver, journaling, dedup, hilos e I/O de
archivos; C11 para el códec (VM ZPAQL, predictor, coder, hashes, postprocesadores);
asm x86-64 para las rutas calientes y el JIT. Límite único: `include/zq.h`.

**Motivos:** el códec necesita control exacto de tipos y aritmética (bit-exactitud) y
se beneficia del asm; el archiver es lógica de estado con strings, hilos y ficheros,
donde Pascal es mucho más productivo. El core no conoce archivos ni hilos: sólo
callbacks `zq_reader`/`zq_writer`.

## ADR-003 — Desenrollado de errores

**Decisión:** `zq_error()` copia el mensaje y hace `longjmp` a un frame instalado por
el punto de entrada del core (thread-local). El host nunca ve un `longjmp`: cada
función del core devuelve estado y `zq_last_error()`.

**Motivos:** upstream lanza `std::runtime_error`/`bad_alloc` y `main` lo imprime;
atravesar frames de Pascal con `longjmp` no es seguro. Todos los saltos ocurren entre
frames C.

## ADR-004 — Multihilo y bit-exactitud

**Decisión:** la bit-exactitud se exige para el mismo `-t` que upstream. El orden de
escritura de bloques en `-m4/-m5` con `-t>1` depende del planificador; el port debe
reproducir la misma politica (`CompressJob`, `writeThread`, `Semaphore`), no sólo el
formato.

**Verificación:** matriz `-t1` primero, y luego `-t2/-t4/-t8` comparando bytes con
upstream.

## ADR-005 — Divergencias toleradas

- Un `.zpaq` corrupto produce un mensaje de error limpio (exit 1) donde upstream a
  veces aborta por `assert`. Se documenta en `docs/DEVIATIONS.md` a medida que
  aparezcan; se prioriza no crashear ni corromper salida.
- Los mensajes de error de ejecución ZPAQL difieren en el texto ("Bad ZPAQL opcode"
  del JIT vs "ZPAQL execution error" del intérprete), pero coinciden en exit code y
  en el momento de fallar.
