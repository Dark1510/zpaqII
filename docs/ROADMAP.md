# zpaqII — backlog for phase 2

The port (phase 1) is done: archives, extracted files, exit codes, console
output and speed match zpaq 7.15 (see `PLAN.md`, `JIT.md`, `DEVIATIONS.md`).
This is the list of work agreed for phase 2, taken from what zpaqfranz and
zpaq-std add on top of zpaq.

Order is by value, not by size.  Every item must keep phase 1 guarantees:
archives readable by plain zpaq 7.15 unless the item says otherwise, bit-exact
output where it applies, and a differential test against the oracle.

## 1. External codecs with a ZPAQL decoder per block

The most valuable format-level idea (zpaq-std's `-ma:algo:N`): the block stores
the compressed payload plus a *decoder written in ZPAQL inside the block*, so
**plain zpaq 7.15 can still extract it** (only slower: it runs the ZPAQL), while
we decode natively.

* Start with one or two codecs (zstd and LZ4 are the obvious pair: one for
  ratio, one for speed).
* Needs: a ZPAQL encoder for the decoder stub (the PCOMP), block framing
  compatible with 7.15 (`findBlock`/`findFilename`/`readComment`), a native
  fast path keyed by the codec name in the block's comment, and a fallback that
  runs our ZPAQL VM.
* Acceptance: an archive we write with `-ma:zstd:3` extracts identically with
  zpaq 7.15, with zpaq-std and with us; archives written by zpaq-std extract
  byte for byte with our port (already true today, keep it true both ways).

## 2. Parallel fragmenter (`-turbo`) and hardware SHA

Two independent `add`-side speedups, both keep the archive byte-identical:

* **Parallel fragmenter**: today one thread cuts and hashes fragments in file
  order; zpaqfranz cuts files >= 4 MiB in several threads and still appends the
  fragments in the original order.  Measured target: the same archive, faster
  `add` on large files.
* **Hardware SHA-1/SHA-256** (SHA-NI on x86-64, ARMv8 crypto extensions) behind
  a runtime CPU check, with the portable code as fallback.  This touches the
  fragment hash and the block hash, i.e. the hot loop of every `add`.

## 3. Configurable integrity and verification commands

zpaq stores one SHA-1 per fragment and per block.  zpaqfranz adds CRC-32 +
XXHASH64 (chunked) by default and a menu of hashes, plus commands to check
them (`t`, `v`, `versum`, `testbackup`, `-checktxt`, `-hashdeep`).

* Store extra digests next to the SHA-1 without breaking 7.15: the natural
  place is the file's attribute blob or a "virtual file" entry inside the
  archive (zpaqfranz's `VFILE-*`), which 7.15 reads as an ordinary file.
* Add a verification path that checks all the digests present (fast ones
  first), and a way to export/import hash lists.
* Keep the format rule: plain zpaq must still extract, and must still verify
  the SHA-1 it knows.

## 4. Compatibility levels and exit-code hygiene

* **`-715` mode**: force the exact 7.15 behaviour and byte output (zpaqfranz
  has it; verified there to produce byte-identical archives).  Useful as a
  safety switch and as a test oracle inside our own program.
* **Exit-code hygiene**: 7.15 returns 0 for several failures (zpaq-std fixed
  part of this, its issue #347: a stray word after the switches, an unknown
  command, a command without an archive all exited 0).  Audit every path so
  that "wrote nothing" is never exit 0.
* Add a `dump`-style command that reports the archive's compatibility level
  (plain 7.15 / our extensions) so a user knows what opens it.

## 5. Optional platform extras (only if there is demand)

These are zpaqfranz features tied to a platform or a niche; they are *not*
planned unless a concrete need appears:

* Sector-level imaging (`image`) and VSS-style backups on Windows.
* Alternate data streams, ACLs and long-path handling on Windows.
* ZFS snapshot integration (`zfsadd`, `zfsbackup`, `zfsreceive`).
* An authenticated outer container (zpaqfranz's FRANZEN, ChaCha20-Poly1305) —
  interesting because it fixes the unauthenticated-AES problem (see
  `LIMITATIONS.md`), but it makes the file *not* a plain `.zpaq`.
* Remote transports (SFTP/SSH) — better left to rsync, as zpaqfranz's own
  README suggests for append-only copies.

## Parked (recorded, not worked on for now)

These are limitations that the forks themselves have *open* (see
`LIMITATIONS.md` §2.3).  They are kept here so they are not rediscovered, but
they are **not** part of phase 2 unless the plan changes:

* SHA-1 as the fragment identity in the format ("XOR SHA-1 to VFILE", "a new
  format without SHA-1" — zpaqfranz's own TODO, decided not worth it).
* Fast listing without storing a history inside the archive (the README of
  zpaqfranz calls it its main defect; `-fast` is a workaround that costs space).
* The general speed of the CM models (2016) — we only work on the edges
  (item 2), not on new models.
* zpaq-std's open Windows bug: `-ma:hs` and `-ma:lzfse` crash on `_WIN32`
  (`memset` with a garbage size, root cause unknown, rejected up front there).
* The four methods that write different bytes on 32-bit builds.
* Experimental codecs whose encoder may still change (`bz2`, `sylz`, `sylzec`,
  `pja`).
* Packaging (FreeBSD port) and the wiki.
* Authenticated container inside the `.zpaq` format: out of scope, see below.

## Execution order

Proposed sequence once this list is approved (each phase ends with its tests
green and a commit, as in phase 1):

| phase | item | why here |
|---|---|---|
| 2.1 | 2 — parallel fragmenter + hardware SHA | pure speed, archive stays byte-identical, no format change: the safest first win |
| 2.2 | 4 — exit codes + `-715` | small; the audit is useful now and `-715` becomes the safety net that lets the *default* behaviour evolve later (items 1 and 3) without losing 7.15 equivalence |
| 2.3 | 3 — configurable integrity and verification | first format extension; needs the `VFILE`/attribute mechanism and the digest plumbing |
| 2.4 | 1 — external codecs with a ZPAQL decoder per block | the biggest; it can reuse the ZPAQL emitter and the block-comment parsing from 2.3 |
| 2.5 | 5 — optional platform extras | only on concrete demand |

**Decision to take before 2.2:** whether our *default* stays 7.15-exact (with the
improvements opt-in) or adopts the forks' defaults (stricter exit codes, unknown
switches ignored).  Either way `-715` must reproduce 7.15 byte for byte and
behaviour for behaviour.

## Out of scope (decided, not pending)

* Changing the archive format to drop SHA-1 or to add authenticated encryption
  inside the container: it would break 7.15 compatibility, which is the whole
  point of this port.  See `LIMITATIONS.md` §1 and §2.
