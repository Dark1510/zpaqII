# Portability plan

Target platforms (agreed):

| id | OS | CPU | notes |
|---|---|---|---|
| `linux-x86_64` | Linux | x86-64 | reference platform, where everything is measured |
| `linux-aarch64` | Linux | ARM64 | no x86 JIT: interpreter unless we add an ARM64 backend |
| `linux-x86` | Linux | i386 | 32-bit; upstream still supports it |
| `windows-x86_64` | Windows 7+ | x86-64 | 64-bit build; ADS support comes later |
| `windows-x86` | Windows 7+ | i386 | 32-bit build |
| `macos-aarch64` | macOS | Apple silicon | no JIT on first round (see below) |

## Verified today (`make cross`)

`tools/cross.sh` compiles the whole core for each target and runs the tests on
the targets this machine can execute:

| target | compile | tests |
|---|---|---|
| Linux x86-64 (native) | 23 objects, 0 warnings | vectors_e1 identical to the oracle, fragchain 50/0 |
| Linux i386 (`gcc -m32`) | 23 objects | vectors_e1 identical to the oracle |
| Linux arm64 (cross + `qemu-aarch64`) | 23 objects | vectors_e1 identical to the oracle, fragchain 50/0 |
| Windows x86-64 (mingw, static) | 23 objects | run by hand on a Windows 10 VM: vectors_e1 identical |
| Windows x86 (mingw, static) | 23 objects | run by hand on a Windows 7 SP1 VM: vectors_e1 identical |

The JIT is now selected by the target, not by a build flag: `zq_internal.h`
defines `ZQ_NOJIT` unless the target is x86-64 with a System V ABI, so Windows,
macOS and every ARM build use the interpreter automatically (byte-identical
output, see `JIT.md`) and `jit.c` no longer includes `<sys/mman.h>` there.

## What is already portable

* **The whole codec** (`src/core/*.c`): SHA-1/256, AES-CTR, scrypt, ZPAQL VM and
  compiler, predictor, coder, container, LZ77/BWT, divsufsort. Plain C11, no
  inline assembly, no POSIX calls. It was written against `zq.h` only.
* **The fragment scanner** (`fragscan.c`), included in the 2.1 work.
* **The Pascal host logic**: archive layer, JIDAC, arguments, output. It is
  already split into units with narrow interfaces (`zqfile`, `zqarchive`,
  `zqjidac`), so the OS-specific parts are concentrated in a few functions.

## What blocks each platform

### 1. JIT (x86-64 machine code)

`jit.c` and `pjit.c` emit x86-64 code and jump into it; `jit.c` allocates
executable memory with `mmap`.

* Guard: `ZQ_NOJIT` already exists (`make NOJIT=1`), and the interpreter is
  byte-exact — the JIT is a performance feature only, never a correctness one.
* The `#include <sys/mman.h>` sits *outside* the `#ifndef ZQ_NOJIT` block, so
  the file does not even compile on Windows today.
* Windows needs `VirtualAlloc`/`VirtualProtect`; macOS ARM64 needs
  `MAP_JIT` + `pthread_jit_write_protect_np`, or simply no JIT.
* The x86-64 JIT also assumes the System V calling convention; the Windows x64
  ABI (shadow space, non-volatile registers) differs and would have to be
  handled if we want JIT on Windows.

**Decision for the first round:** JIT stays Linux x86-64 only. Everywhere else
the core builds with `ZQ_NOJIT` and behaviour is identical (measured: within
25 % of the JIT on the models). An ARM64 JIT is a separate, later item.

### 2. Host OS layer — done for Linux and Windows x86-64

`zqfile.pas` is now the only unit with system calls.  It has two
implementations selected by `{$IFDEF WINDOWS}`:

| primitive | Unix | Windows |
|---|---|---|
| file metadata | `fpLStat` (does not follow links) | `GetFileAttributesExW`; a reparse point is neither file nor directory, so the scanner skips it like upstream |
| attributes | `'u' + (mode << 8)` | `'w' + (dwFileAttributes << 8)`, exactly upstream's encoding |
| date | `st_mtime` | `ftLastWriteTime` (UTC), same `YYYYMMDDHHMMSS` |
| set date/attrs | `fpUtime` + `fpChmod` | `SetFileTime` + `SetFileAttributesW` |
| open read/write | `fpOpen` | `CreateFileW` |
| mkdir | `fpMkdir` | `CreateDir` |
| clock | `fpGetTimeOfDay` | `GetTickCount64` (only differences are used) |
| CPU count | `/proc/cpuinfo` | `GetSystemInfo` |

`zqjidac.pas` no longer uses `BaseUnix`/`Unix` at all; its `OpenIn`/`OpenOut`
delegate to `zqfile`.

Verified: `make win64` produces `build/win64/zpaqii.exe` (core as `zqcore.dll`
+ the Pascal host, cross-compiled with FPC's win64 units), and in a Windows 10
VM:

* `zpaqii.exe add` of a small tree, then **upstream 7.15 on Linux extracts it
  correctly**;
* an archive written by upstream on Linux extracts correctly with
  `zpaqii.exe`.

Windows x86: the core builds and its E1 vectors were run in a Windows 7 SP1
VM; the 32-bit *host* needs FPC on Windows itself, because Debian ships no
i386-win32 units (`tools/win.sh win32` builds the core and the tests).

Still missing on the host: macOS arm64 (no hardware here; the code has no
Unix-specific calls left outside `zqfile`, but it must be built and tested on a
Mac), and the Windows-only extras (alternate data streams) of §2.5.

### 3. Host OS layer (old plan, kept for the record)

`zqfile.pas` and `zqjidac.pas` use `BaseUnix`/`Unix`:

* `lstat` (symlink detection while scanning) → Windows `GetFileAttributes`.
* Unix mode bits in the attribute blob and `chmod` on extract → Windows
  read-only flag today; ADS/ACLs are a phase-2.5 item.
* `Now`/UTC conversion, `FileAge`/`FileSetDate` → already `SysUtils`, fine.
* Path separator: the archive stores `/`; scanning and extracting must
  translate on Windows (upstream does the same).
* Console: `WriteLn` is fine, but colour/`isatty` checks must be guarded.
* Threads: `SyncObjs` (TEvent/TCriticalSection) is cross-platform; the C core
  has no threads of its own (the host owns the pool), so nothing to port there.

Plan: one `zqos.pas` unit with the ~10 primitives the host needs
(`lstat`, `chmod`, `path_is_dir`, `scan_dir`, `attr_to_blob`,
`blob_to_attr`, `now_utc`, `conservative_rewrite`), with `{$IFDEF WINDOWS}`
inside it and nothing platform-specific anywhere else.

### 3. Build system

The `Makefile` assumes a Unix toolchain: `ar`, `-lzqcore`, `-lc`, `.so`/`.a`
naming with `{$LinkLib}`.

Plan:

* Keep the Makefile for Linux, add `build.ps1`/`Makefile.mingw` for the mingw
  toolchain (FPC + gcc from mingw-w64), producing `zqcore.dll` or a static
  `libzqcore.a`.
* Use the `fpc` cross-compilers for `i386-win32`/`x86_64-win64` and
  `aarch64-darwin`/`aarch64-linux` where available; otherwise build natively in
  CI (GitHub Actions matrix: ubuntu-latest, ubuntu-24.04-arm, windows-latest,
  macos-14).
* The `.lpr` must stop passing `-k-l` Unix link flags on Windows.

## Build entry points

| command | what it does |
|---|---|
| `make` / `make test` | Linux x86-64 |
| `make cross` | core for x86-64, i386 (`-m32`), arm64 (qemu), Windows x64/x86 |
| `make win64` | `build/win64/zpaqii.exe` + `zqcore.dll` |
| `make win32` | 32-bit core, tests and DLL (no host) |

## Order

1. **Now (2.1):** keep the new code portable — `fragscan.c` and the SHA
   backends use only `zq.h` plus `<stdint.h>`/`<string.h>`; CPU detection is
   per-platform but isolated in `cpu.c` (see below), with a portable C fallback.
2. **After 2.2:** the `zqos.pas` split + `ZQ_NOJIT` on non-Linux-x86-64, so the
   project *builds* everywhere even if a feature is missing.
3. **Then:** the CI matrix, so "builds on X" is a test result and not a claim.
4. **Later (2.5, only with demand):** Windows ADS/ACLs, ARM64 JIT.

## CPU feature detection (needed by 2.1)

One small `src/core/cpu.c` with `zq_cpu_flags()`, cached in a static:

| platform | how |
|---|---|
| Linux x86-64 | `cpuid` (leaf 7, EBX bit 29 = SHA-NI), `__builtin_cpu_supports` where available |
| Windows x86/x64 | `cpuid` via `<intrin.h>`; msys/mingw and MSVC both provide it |
| Linux ARM64 | `getauxval(AT_HWCAP)` → `HWCAP_SHA2` (and `HWCAP_SHA1` for the SHA-1 extension) |
| macOS ARM64 | `sysctlbyname("hw.optional.arm.FEAT_SHA256")` |
| fallback | no features: portable C |

The portable C implementations stay in the binary in every case; the dispatch
is a function pointer chosen once.

## Verification

* Behaviour must not depend on the platform: the matrix (`make matrix`) and the
  bit-exact tests run per platform in CI; archives written on one platform must
  extract identically on another (that test already exists for Linux).
* Byte output must not depend on the SHA backend: a unit test hashes the same
  buffers through `portable` and through `hw` and compares.
