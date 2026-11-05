# Known deviations from zpaq 7.15

## Cosmetic differences

Everything the commands *produce* (archives, extracted files, exit codes,
stdout, stderr) matches upstream, with three exceptions that cannot match:

* **Banner.** Both print `zpaq v7.15 journaling archiver, compiled <date>`, but
  the date is each binary's build date (`__DATE__` vs FPC's `%DATE%`, different
  formats).
* **Timing line.** The `N.NNN seconds (all OK)` trailer is measured, so it
  differs.  The test suite normalises both lines.
* **stdout/stderr interleaving.** Stream *contents* are identical, but when
  both are redirected to the same file the order can differ: FPC line-buffers
  `Output`, so a `perror`-style message (e.g. `nope.txt: No such file or
  directory` during `add`) appears in place, while upstream's fully buffered
  stdout pushes it to the front.

## Scope

* Linux x86-64 only.  The JITs emit x86-64 for the System V ABI; Windows
  (alternate data streams, `FSCTL_SET_SPARSE`, the Win64 register arguments)
  is not implemented.
* `zpaq`'s release build hides the "Advanced options" part of the usage text
  (`#ifndef NDEBUG`), so the port prints the same short text.

* `makeConfig` for a level-1 method with a block size above 12 (`x13,1`…,
  r-bits above 8): 7.15 generates a config its own compiler then rejects
  ("Config line N at X: number too high"); the port raises "number too high"
  while generating it.  The archiver refuses such block sizes in both
  ("blocksize must be 0..11"), so only the library sees this.  Block size 12
  is generated exactly like 7.15 (until phase B it silently took the wrong
  template).

## Fixed during the port (kept here as a record)

These were real divergences found by differential testing and are now closed:

| area | was | is |
|---|---|---|
| exit codes on corrupt archives | 0/1 regardless of skipped blocks | like upstream: `list` always 0, `extract` 1 for incomplete files, 2 for fatal errors, `add` 1 on read errors |
| transaction date | local time (`DateTimeToUnix(Now)`) | UTC, identical to upstream's `gmtime` |
| `add -to`, `-only`, `-not`, symlinks | ignored / followed | applied, `lstat` scan like upstream |
| streaming add | command-line order, no output | sortkey order, full progress output |
| streaming extract | silent, overwrote multi-segment files | prints like upstream, appends segments |
| `extract` over existing files | overwrote them | skips without `-force`, `=` when identical |
| `equal()` for `-force` | decompressed and compared bytes (failed on preprocessed blocks) | compares fragment SHA-1s |
| `-summary` | no duplicate detection | `^` entries and the real count |
| `add -until` | ignored | rolls the archive back |
| missing archive | empty listing, exit 0 | `perror` + exit 2 |
| per-file I/O errors | aborted the command | reported and skipped, like upstream |

## Not deviations (upstream behaviour reproduced on purpose)

See `docs/JIT.md` for the JIT quirks (runs of identical swap bytes, repeated
`predict()` without `update()`, 32-bit stores into `size_t` fields, code
invalidated by `init()`).
