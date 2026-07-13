#!/usr/bin/env python3
"""Extract + freeze the QuantumOS v1 ABI contract (ADR-0020).

Reads COMPILER-MEASURED ABI values out of the .abi_ents section of the compiled
probe objects (user/abi_probe.c, kernel/src/abi_probe_kern.c) and either emits or
checks a committed golden file. The values are what the compiler actually laid
out (macro expansions, sizeof) -- never parsed from a source comment or a
_Static_assert literal, so a struct edited together with its size-claim comment
still reddens the gate.

The section is a packed array of 64-byte records: a 56-byte NUL-padded ASCII
name followed by a little-endian u64 value. We pull the raw section bytes with
`objcopy -O binary` (no fragile objdump-text parsing) and unpack them.

Modes:
  extract OBJ...                    print the canonical table to stdout
  emit --out GOLDEN OBJ...          write the canonical table to GOLDEN
  check --golden GOLDEN OBJ...      diff extracted vs GOLDEN; exit 1 on any drift

`check` also runs the twin cross-check (a logical name present on both the user
and kern rings must carry the SAME value) and refuses an empty extraction, so a
probe that failed to compile or a truncated read can never pass.

Stdlib-only by contract (the CI integration lane forbids pip).
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

REC = 64      # bytes per record
NAMELEN = 56  # NUL-padded ASCII name; the remaining 8 bytes are the LE u64 value


def read_section(obj):
    """Return {name: unsigned_u64} for every record in obj's .abi_ents section."""
    fd, tmp = tempfile.mkstemp(suffix=".abisec")
    os.close(fd)
    try:
        r = subprocess.run(
            ["objcopy", "-O", "binary", "--only-section=.abi_ents", obj, tmp],
            capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("objcopy failed on %s: %s" % (obj, r.stderr.strip()))
        with open(tmp, "rb") as f:
            raw = f.read()
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    if len(raw) == 0:
        sys.exit("%s: .abi_ents is empty (probe failed to compile?)" % obj)
    if len(raw) % REC != 0:
        sys.exit("%s: .abi_ents is %d bytes, not a multiple of %d" % (obj, len(raw), REC))
    ents = {}
    for i in range(0, len(raw), REC):
        rec = raw[i:i + REC]
        name = rec[:NAMELEN].split(b"\x00", 1)[0].decode("ascii")
        (value,) = struct.unpack("<Q", rec[NAMELEN:REC])
        if name in ents and ents[name] != value:
            sys.exit("%s: duplicate name %s with differing values" % (obj, name))
        ents[name] = value
    return ents


def collect(objs):
    merged = {}
    for o in objs:
        for name, value in read_section(o).items():
            if name in merged and merged[name] != value:
                sys.exit("duplicate name %s across objects with differing values" % name)
            merged[name] = value
    if not merged:
        sys.exit("no ABI entries extracted from %r" % (objs,))
    return merged


def render(v):
    """u64 -> stable decimal; values with the sign bit set render as int64 so
    errnos read as -1, -11 rather than a giant unsigned constant."""
    return str(v - (1 << 64)) if v >= (1 << 63) else str(v)


def canonical(ents):
    return "".join("%s = %s\n" % (n, render(ents[n])) for n in sorted(ents))


def twin_errors(ents):
    """A logical name (KIND:X, ring prefix stripped) present on both rings must
    carry the same value."""
    by_logical = {}
    for name, v in ents.items():
        ring, rest = name.split(":", 1)
        by_logical.setdefault(rest, {})[ring] = v
    errs = []
    for rest, rings in sorted(by_logical.items()):
        if "user" in rings and "kern" in rings and rings["user"] != rings["kern"]:
            errs.append("TWIN MISMATCH %s: user=%s kern=%s"
                        % (rest, render(rings["user"]), render(rings["kern"])))
    return errs


def main():
    ap = argparse.ArgumentParser(description="QuantumOS v1 ABI freeze gate (ADR-0020)")
    ap.add_argument("mode", choices=["extract", "emit", "check"])
    ap.add_argument("objs", nargs="+", help="compiled probe object files")
    ap.add_argument("--golden", help="committed golden file (check mode)")
    ap.add_argument("--out", help="golden file to write (emit mode)")
    args = ap.parse_args()

    ents = collect(args.objs)
    terrs = twin_errors(ents)
    text = canonical(ents)

    if args.mode == "extract":
        sys.stdout.write(text)
        if terrs:
            sys.stderr.write("\n".join(terrs) + "\n")
            return 1
        return 0

    if args.mode == "emit":
        if not args.out:
            ap.error("emit needs --out")
        if terrs:
            sys.stderr.write("refusing to emit a golden with twin mismatches:\n")
            sys.stderr.write("\n".join(terrs) + "\n")
            return 1
        with open(args.out, "w", newline="\n") as f:
            f.write(text)
        sys.stderr.write("wrote %d entries to %s\n" % (len(ents), args.out))
        return 0

    # check
    if not args.golden:
        ap.error("check needs --golden")
    if not os.path.exists(args.golden):
        sys.stderr.write("FROZEN ABI GATE: golden %s is missing -- run "
                         "`make regen-abi-golden`\n" % args.golden)
        return 1
    with open(args.golden, "r", newline="") as f:
        want = f.read().replace("\r\n", "\n")
    rc = 0
    if terrs:
        sys.stderr.write("FROZEN ABI GATE: user/kern twins disagree:\n")
        sys.stderr.write("\n".join(terrs) + "\n")
        rc = 1
    if text != want:
        rc = 1
        sys.stderr.write("FROZEN ABI GATE: extracted ABI != %s\n" % args.golden)
        want_lines = set(want.splitlines())
        got_lines = set(text.splitlines())
        for line in sorted(got_lines - want_lines):
            sys.stderr.write("  + %s\n" % line)
        for line in sorted(want_lines - got_lines):
            sys.stderr.write("  - %s\n" % line)
        sys.stderr.write("If this change is intended, run `make regen-abi-golden` "
                         "and commit the golden diff.\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
