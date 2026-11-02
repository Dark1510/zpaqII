# Known deviations from zpaq 7.15

## Exit codes on corrupt archives — fixed (E20)

The port used to return 1 for any extraction problem and to count skipped blocks
in the exit code.  It now follows upstream exactly:

| command | exit code |
|---|---|
| `list` | always 0 (it only reads, even if blocks were skipped) |
| `extract` / `x` | 1 when a file could not be extracted completely (missing fragments), 2 for a fatal error (bad archive, wrong key, …) |
| `extract -index F` | 0 once the index is written |
| `extract -repack` | 0 on success |
| `add` | 1 when the archive had read errors |

Two details matter:

* `extract()` rejects a scan that returns no data at all (`sz < 1`) with
  `archive not found`; `list()` does not perform that check, so a truncated
  archive still lists and returns 0.
* skipped blocks are counted only by `add`.  Extraction ignores them (as
  upstream, which does not pass the error counter to `read_archive`), so an
  archive with a damaged block that contains no files still extracts with 0.

`tests/fuzz.py` now treats any disagreement on success/failure as a failure, and
the 204-case run reports **0 failures and 0 exit-code divergences**.

## Transaction dates — fixed (E20)

`add` stamped the transaction with local time (`DateTimeToUnix(Now)` treats the
local `TDateTime` as UTC in FPC).  It now uses `DateTimeToUnix(Now, False)`, so
the C block name carries the UTC date like upstream's `gmtime`-based code, and
archives are identical regardless of `TZ`.  `tests/cli.sh` checks it.

## Not deviations (upstream behaviour reproduced on purpose)

See `docs/JIT.md` for the JIT quirks (runs of identical swap bytes, repeated
`predict()` without `update()`, 32-bit stores into `size_t` fields, code
invalidated by `init()`).
