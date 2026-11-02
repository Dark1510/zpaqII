
## Exit codes on corrupt archives

`zpaq`'s exit code for a damaged archive depends on how many blocks it skipped
and on the command (it can be 1 or 2, and `list` always returns 0). The port
currently returns 1 (extract) or 0 (list) regardless of the number of skipped
blocks. `tests/fuzz.py` reports these as *exit-code divergences* (not
failures): the hard requirements it enforces are no crashes and identical
listed file sets.
