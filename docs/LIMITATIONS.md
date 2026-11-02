# What zpaq still cannot do (limitations not solved by any fork)

Study of zpaq 7.15 and of its two active forks, `fcorbelli/zpaqfranz` 65.17o and
`YadeWira/zpaq-std` 2.2.0.  "Solved" means really solved in the format or in the
program, not worked around.  Verified against `zpaq.pod` (the 7.15 manual), the
7.15 source, and the forks' `README.md`, `TODO.md`, `docs/TODO.md` and
`CHANGELOG.md`.

Legend for "who": **7.15** = base, **ZF** = zpaqfranz, **ZS** = zpaq-std,
**nadie** = still open.

## 1. The format itself

### 1.1 SHA-1 is the identity of a fragment
The H block stores `SHA1 || size` per fragment and `add` deduplicates by
looking the hash up; a match reuses the stored fragment without reading the
data again.  SHA-1 has practical chosen-prefix collisions (SHAttered 2017,
Leurent–Peyrin 2020) and the format has no second hash on that path.

* Consequence: an attacker who can choose file contents can craft two
  fragments with the same SHA-1; the second is *not stored* and the archive
  silently returns the first when the second is extracted.
* Nobody fixed it: changing the fragment identity breaks every existing
  archive.  zpaqfranz `TODO.md`: *"XOR-ing SHA-1 to VFILE. Requires a lot of
  work to fix SHA-1 collisions"* (open) and *"New file format without SHA-1.
  Backward compatibility will be broken. Not sure => not worth"* (open, by
  decision).  zpaq-std inherits it.  zpaqfranz's `collision` command only
  *detects* duplicate hashes in an archive; it cannot prevent them.
* Who: **nadie**.

### 1.2 Encryption is not authenticated
The archive is encrypted with AES-256-CTR (`AES_CTR` in libzpaq) and the key
comes from `scrypt(password, salt, N=2^14, r=8, p=1)`.  There is **no MAC and
no AEAD**: CTR gives confidentiality only.

* Consequences: ciphertext is malleable (bit flips are undetected at the crypto
  layer; the per-fragment SHA-1 catches most damage *after* decryption, but the
  last partial block and the metadata do not have that protection, and a
  targeted attacker can splice whole blocks from one archive into another with
  the same key); no protection against truncation or reordering at the
  container level; no key rotation (the salt is fixed at creation and the KDF
  parameters cannot be upgraded without breaking the format); no way to check
  the password without attempting to decrypt.
* zpaqfranz added an **outer** container, FRANZEN (ChaCha20-Poly1305, CRC-32 per
  block, parallel verification), and a `checkpassword` command — but the inner
  `.zpaq` remains unauthenticated, the outer layer makes the file *not* a plain
  `.zpaq`, and zpaq-std's open build does not ship FRANZEN at all.
* Who: **ZF parcial** (capa externa), **ZS no**.

### 1.3 No redundancy, no repair
There is one copy of every block and one copy of every H/I block.  A damaged D
block loses every file that has a fragment in it; a damaged transaction loses
that version's index.  There is no parity, no erasure code, no self-healing.

* What exists: `zpaqfranz trim` (drop an incomplete last transaction), `skip`
  damaged blocks during extraction ("extract as much as possible"), and
  `testbackup`/`p` to *detect* damage early.  Repairing is out of reach: the
  data is simply not there.
* Who: **nadie** (only detection + prevention, and the practical advice is "keep
  two copies": zpaqfranz's `quadra` checks that two archives are identical).

### 1.4 Metadata is thin
From `zpaq.pod`: only name, size, date and a Unix attribute blob (mode bits) are
stored.  **Owner, group, ACLs, extended attributes and last access time are not
saved.  Symbolic links are neither saved nor followed.  Hard links are followed
as ordinary files** (their content is duplicated).  Devices, FIFOs and sockets
are not saved.  Alternate data streams only in the 64-bit Windows build.

* zpaqfranz works around parts of this on Windows (ADS, ACLs via `resetacl`,
  long paths, 255-char fixes) and skips `.zfs`/QNAP/Synology internals, but the
  format still has nowhere to put xattrs, ACLs or link targets.
* Who: **nadie** (format limitation).

### 1.5 Hard limits
| limit | value | where |
|---|---|---|
| archive size | ~250 TB, 4 billion files | `zpaq.pod` |
| fragment id | 32 bits | H/I block (`puti(..., 4)`) |
| fragment size | < 2 GiB (`f>0x7fffffff`) | `zpaq.cpp` read_archive |
| block (D) size | < 2 GiB (`log_blocksize` 20..31) | `-method NB` |
| file name | < 65536 bytes | `read_archive`, `string too long` |
| attribute blob | < 65536 bytes | `attr too long` |
| metadata (H/I) block | < 1.5 GB of RAM to parse | `index block requires too much memory` |
| archive parts | counted by name; no limit in practice | `too many archive parts` |
| ZPAQL memory | H and M <= 32 bits each | libzpaq `H too big` |

These are inherited unchanged by both forks.

### 1.6 No random access
To get one byte of a file, the whole block that holds its fragment must be
decompressed.  7.15 stops at the last fragment it needs ("blocks are only
decompressed up to the last used fragment"), which is an optimisation, not
random access.  There is no per-file offset index, no range extraction, no
"open the archive as a file system".

* zpaqfranz's `w` command decompresses large files in chunks with bounded RAM;
  `mem`/`-frugal` trade RAM for speed.  None of that changes the block model.
* Who: **nadie**.

### 1.7 "Append-only" is not guaranteed by 7.15
The transaction header (C block) is written with a placeholder data size and
**rewritten in place** at the end, so `add` seeks backwards.  That is why
`rsync --append` could corrupt an archive written by 7.15.

* zpaqfranz added `-append` (two passes, output written only forward, and the
  two passes must agree or nothing is kept) and `-appendoutput` (a delta that
  is glued with `cat`).  zpaq-std inherits both.  Our port reproduces 7.15
  (in-place rewrite) for bit-exactness.
* Who: **ZF/ZS resuelven** con un modo nuevo; el formato sigue igual.

### 1.8 `-index` is a second-class citizen
The index file `.zpaq`-like can only be *listed*; it cannot be extracted or
updated, it must be kept in sync by the user, and `-index` cannot be combined
with `-until` or with streaming archives.

* Both forks keep the rule.  zpaqfranz's `-fast` (a virtual `VFILE-info` and a
  history inside the archive) is a different workaround for a different problem
  (fast listing).
* Who: **nadie**.

### 1.9 Deduplication is per archive
Fragments are deduplicated inside one archive only.  Two archives of the same
data store it twice; the format has no way to reference a fragment in another
file (that is why zpaqfranz uses multipart + one index instead of several
archives).

* Who: **nadie** (a design constraint, not a bug).

### 1.10 The compression model is from 2016
`-m1`..`-m5` are the same models Mahoney tuned a decade ago.  In practice
`-m5` sits around bzip2-level ratio at a fraction of the speed of a modern CM
compressor, and the format cannot adopt a new internal model without new
component types (which 7.15 would not understand).

* zpaq-std's answer is not a better model but **external codecs** (`-ma:zstd`,
  `-ma:brotli`, `-ma:lzma`, ... 21 + 3 experimental), each block carrying its
  own ZPAQL decoder so plain 7.15 still extracts.  That is a real improvement,
  but the *internal* models are untouched, and a codec is chosen per archive,
  not per block type.
* Who: **ZS parcial** (por la vía externa).

## 2. The 7.15 program

### 2.1 Argument handling and exit codes
Measured on the three binaries (7.15, zpaqfranz 65.17o, zpaq-std 2.2.0):

| command line | 7.15 | ZF | ZS |
|---|---|---|---|
| misplaced word after a switch (`a x.zpaq f -m5 basura`) | usage, **1** | `00569`, **2** | `00569`, **2** |
| unknown command (`foo bar`) | usage, **1** | **2** | **2** |
| command without archive (`x`) | usage, **1** | **2** | **2** |
| bad value for a switch (`-fragment abc`) | **0** (atoi -> 0, silently) | **2** | **2** |
| unknown switch (`l a.zpaq -zzz`) | usage, **1** | ignored, **0** | ignored, **0** |

So the "exit 0 after writing nothing" bug (zpaqfranz issue #347) is fixed in
both forks (they answer 2), while 7.15 still accepts a bad value in silence and
turns an unknown switch into a usage error.  Note that the forks also *diverge*
from 7.15 in the other direction (they ignore unknown switches where 7.15
fails), so a script cannot rely on the same code across all three.  Our port
reproduces 7.15 on purpose.

* Who: **ZF/ZS** (distinto de 7.15, no "mejor" en todos los casos);
  **nosotros reproducimos 7.15**.

### 2.2 Listing cost grows with the number of versions
`l` must read the whole index chain: with thousands of versions it is slow (the
zpaqfranz README calls it "the main defect").  Its `-fast` mode stores a
`VFILE-info` + history inside the archive and then lists from that, at the cost
of space in every update, and `-nofast` goes back to the slow path.  The
format's own cost is unchanged.

* Who: **ZF/ZS con workaround** (espacio extra); el formato sigue igual.

### 2.3 Extraction does many seeks
Deduplicated fragments make extraction jump between blocks; on spinning disks
that costs a lot (the README says so).  `w` (chunked extraction into RAM) and
`-frugal` reduce the pain; the block model is the cause.

* Who: **ZF/ZS con workaround**.

### 2.4 Speed
Real-world `add` runs at 80-200 MB/s depending on data and hardware (README).
The forks improved the *edges* (hardware SHA-1/SHA-2, parallel fragmenter
`-turbo`, faster stored blocks) but the CM models themselves are as slow as in
2016.

### 2.5 Memory
Documented in `zpaq.pod`: up to **8× the block size per thread** for levels up
to 4 and **16×** for level 5.  A 64 MiB block at `-m5` is around 1 GB per
thread, which is why the default block grows with the level and why zpaqfranz
has `-frugal` for extraction.

### 2.6 Nothing is ever pruned
`add` only appends.  There is no retention policy, no way to drop old versions
or free the space of deleted files without rewriting the archive.  zpaqfranz
added `n` (keep the newest X), `crop` (drop the last versions), `purge`/`repack`
(rebuild with selected versions, recompressing).  In 7.15: **nada**.

* Who: **ZF/ZS**.

### 2.7 The password cannot be changed in place
The salt is fixed when the archive is created and the key is derived from it;
changing the password means rewriting the whole archive.  zpaqfranz's
`password` command does exactly that (and only for a single-file archive).

* Who: **ZF** (reescritura); el formato no lo permite.

### 2.8 Dates
Stored with one-second resolution and always in UTC (`zpaq.pod`: "time zone
(UT), not local time").  No sub-second, no time zone, no creation/access time
(Windows ADS aside, and zpaq-std notes that on Windows it stores creation and
access dates as an extension).

### 2.9 The original is unmaintained
Mahoney retired; 7.15 (2016) is the last release.  That is the reason both forks
exist.  Not a technical limitation, but it is why every fix above lives in a
fork.

## 3. What the forks themselves still have open

From their own files (not my inference):

| item | source | state |
|---|---|---|
| SHA-1 collisions in the format ("XOR SHA-1 to VFILE") | zpaqfranz `TODO.md` | open, mid priority |
| new format without SHA-1 | zpaqfranz `TODO.md` | open, "not sure => not worth" |
| fast listing without storing a history | README ("main defect") | worked around, not solved |
| `-ma:hs` and `-ma:lzfse` crash on the Windows build (memset with a garbage size) | zpaq-std `docs/TODO.md` | **open**, mitigated by rejecting the switch on `_WIN32` |
| 32-bit builds write different bytes for `brotli`, `lz6:0`, `lz4f`, `lz5f` | zpaq-std README | known, documented |
| FreeBSD package, wiki | both `TODO.md` | open, low priority |
| FRANZEN is not in the open build | zpaq-std README/CHANGELOG | by design (it now fails loudly) |
| experimental codecs' encoders may still change (`bz2`, `sylz`, `sylzec`, `pja`) | zpaq-std README | open |

## 4. Summary

| limitation | 7.15 | ZF | ZS | resuelto |
|---|---|---|---|---|
| SHA-1 como identidad (colisiones) | sí | no | no | **no** |
| cifrado sin autenticar | sí | capa externa | no | **no** (en `.zpaq`) |
| sin redundancia/reparación | sí | detecta | detecta | **no** |
| sin xattrs/ACLs/symlinks/hard links | sí | parte Windows | no | **no** |
| 250 TB / 4 G ficheros / 2 GB bloque | sí | igual | igual | n/a (límite) |
| sin acceso aleatorio | sí | no | no | **no** |
| append-only roto | sí | `-append` | `-append` | sí (modo nuevo) |
| `-index` de segunda clase | sí | igual | igual | **no** |
| dedupe sólo intra-archivo | sí | igual | igual | **no** |
| modelos de compresión de 2016 | sí | `-m6`, zstd | códecs externos | parcial |
| exit codes / argumentos | 0 o 1 según el caso | 2 en los errores | 2 en los errores | parcial (los forks divergen de 7.15) |
| listado lento | sí | `-fast` (espacio) | `-fast` | workaround |
| seeks en extracción | sí | `w`, `-frugal` | igual | workaround |
| memoria 8×/16× bloque | sí | `-frugal` | igual | workaround |
| sin poda/limpieza | sí | `n`/`crop`/`purge` | igual | sí (ZF) |
| contraseña no cambiable in situ | sí | `password` (reescribe) | igual | parcial |
| fechas UTC 1 s | sí | `-utc` opcional | igual | **no** |
| mantenimiento | parado | activo | activo | sí |
