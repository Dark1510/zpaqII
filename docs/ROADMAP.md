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

## Out of scope (decided, not pending)

* Changing the archive format to drop SHA-1 or to add authenticated encryption
  inside the container: it would break 7.15 compatibility, which is the whole
  point of this port.  See `LIMITATIONS.md` §1 and §2.
