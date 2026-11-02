#!/usr/bin/env python3
"""E6a: differential fuzzing of the port against upstream zpaq.

Generates corrupted/truncated variants of valid archives and runs both
implementations on them.  Criteria:
  * the port must never die on a signal (segfault/abort) unless upstream does;
  * when upstream succeeds (exit 0), the port must also succeed;
  * when both succeed, the listed file set must match;
  * extraction must agree on success vs failure (exit code 0 or not).

Usage: fuzz.py <upstream-zpaq> <zpaqii> <workdir> [rounds]
"""
import os
import random
import shutil
import subprocess
import sys

UP = os.path.abspath(sys.argv[1])
PORT = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])
ROUNDS = int(sys.argv[4]) if len(sys.argv) > 4 else 120
TIMEOUT = 60

random.seed(20261101)


def run(binary, args, env=None):
    try:
        p = subprocess.run([binary] + args, capture_output=True,
                           timeout=TIMEOUT, env=env)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", b"", b""


def listed_files(out):
    names = []
    for line in out.decode("latin1").splitlines():
        if line[:2] in ("- ", "+ ", "# ", "= "):
            parts = line.split(None, 4)
            if len(parts) >= 5:
                names.append(parts[4])
    return sorted(names)


def mutate(data, kind):
    b = bytearray(data)
    if kind == 0 and len(b) > 1:
        return bytes(b[:random.randrange(1, len(b))])
    if kind == 1:
        for _ in range(random.randrange(1, 9)):
            if b:
                i = random.randrange(len(b))
                b[i] ^= 1 << random.randrange(8)
        return bytes(b)
    if kind == 2:
        if len(b) > 2:
            i = random.randrange(len(b))
            n = random.randrange(1, max(2, len(b) // 20))
            for j in range(i, min(len(b), i + n)):
                b[j] = 0
        return bytes(b)
    if kind == 3:
        return bytes(b) + bytes(random.randrange(256)
                                for _ in range(random.randrange(1, 64)))
    return bytes(b)


def main():
    os.makedirs(WORK, exist_ok=True)
    os.chdir(WORK)
    corpus = "/mnt/IA_LAB/agentes/zpaqII/upstream-zpaq"
    # deterministic inputs
    with open("text.txt", "wb") as f:
        f.write((b"the quick brown fox jumps over the lazy dog 0123456789\n"
                 * 2000))
    with open("bin.dat", "wb") as f:
        r = random.Random(7)
        f.write(bytes(r.randrange(256) for _ in range(50000)))
    with open("small.txt", "wb") as f:
        f.write(b"small file\n")
    # Fixed mtimes and a frozen clock so the base archives are byte-identical
    # on every run: the mutations (and therefore the whole test) are
    # deterministic.  The clock shim lives next to the port binary.
    fixed = 1767225600  # 2026-01-01 00:00:00 UTC
    for fn in ("text.txt", "bin.dat", "small.txt"):
        os.utime(fn, (fixed, fixed))
    shim = os.path.join(os.path.dirname(PORT), "faketime.so")
    base_env = dict(os.environ)
    if os.path.exists(shim):
        base_env["LD_PRELOAD"] = shim
        base_env["FAKE_TIME"] = str(fixed)
        base_env["ZPAQII_NOW"] = str(fixed)
    else:
        print("note: no faketime.so, base archives depend on the clock")

    bases = {}
    for name, args in (
        ("streaming", ["add", "streaming.zpaq", "text.txt", "bin.dat",
                       "-method", "s4.1.5.0.3.22", "-t1"]),
        ("journal", ["add", "journal.zpaq", "text.txt", "bin.dat",
                     "-m1", "-t1"]),
        ("journal5", ["add", "journal5.zpaq", "text.txt", "small.txt",
                      "-m5", "-t1"]),
    ):
        subprocess.run([UP] + args, capture_output=True, timeout=120,
                       env=base_env)
        try:
            bases[name] = open(args[1], "rb").read()
        except OSError:
            print("cannot create base archive", name)
            return 2

    fails = 0
    cases = 0
    for name, data in bases.items():
        for n in range(ROUNDS):
            kind = n % 4
            mut = mutate(data, kind)
            path = "fuzz.zpaq"
            with open(path, "wb") as f:
                f.write(mut)
            ru, ou, eu = run(UP, ["list", path])
            rp, op, ep = run(PORT, ["list", path])
            cases += 1
            up_crash = ru == "timeout" or (isinstance(ru, int) and ru < 0)
            port_crash = rp == "timeout" or (isinstance(rp, int) and rp < 0)
            if port_crash and not up_crash:
                print("CRASH %s case %d kind %d (up rc=%s) size %d"
                      % (name, n, kind, ru, len(mut)))
                fails += 1
                shutil.copy(path, "crash_%s_%d.zpaq" % (name, n))
                continue
            if up_crash:
                continue
            if ru == 0 and rp != 0:
                print("REJECTED %s case %d kind %d: upstream ok, port rc=%s (%s)"
                      % (name, n, kind, rp, ep.decode("latin1").strip()[:80]))
                fails += 1
                continue
            if ru == 0 and rp == 0:
                lu, lp = listed_files(ou), listed_files(op)
                if lu != lp:
                    print("DIFF %s case %d kind %d: listed %d vs %d files"
                          % (name, n, kind, len(lu), len(lp)))
                    fails += 1

    # extraction fuzzing: the port must not crash and must agree on success
    for name, data in bases.items():
        for n in range(0, ROUNDS, 8):
            mut = mutate(data, n % 4)
            with open("fuzz.zpaq", "wb") as f:
                f.write(mut)
            for d in ("xu", "xp"):
                shutil.rmtree(d, ignore_errors=True)
                os.makedirs(d)
            ru, _, _ = run(UP, ["extract", "fuzz.zpaq", "-to", "xu", "-t1"])
            rp, _, _ = run(PORT, ["extract", "fuzz.zpaq", "-to", "xp"])
            cases += 1
            up_crash = ru == "timeout" or (isinstance(ru, int) and ru < 0)
            port_crash = rp == "timeout" or (isinstance(rp, int) and rp < 0)
            if port_crash and not up_crash:
                print("CRASH extract %s case %d" % (name, n))
                fails += 1
                shutil.copy("fuzz.zpaq", "crashx_%s_%d.zpaq" % (name, n))
                continue
            if up_crash:
                continue
            if (ru == 0) != (rp == 0):
                print("RC extract %s case %d: upstream %s, port %s"
                      % (name, n, ru, rp))
                fails += 1
                continue
            if ru == 0:
                fu = sorted(os.listdir("xu"))
                fp = sorted(os.listdir("xp"))
                if fu != fp:
                    print("FILES extract %s case %d: %s vs %s"
                          % (name, n, fu, fp))
                    fails += 1

    print("fuzz: %d cases, %d failures" % (cases, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
