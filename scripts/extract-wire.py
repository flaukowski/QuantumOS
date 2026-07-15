#!/usr/bin/env python3
"""Extract + freeze the QuantumOS v1 WIRE contract (ADR-0020 lane C).

Two rings, one golden. The GUEST ring is compiler-measured: the probe TU
(user/wire_probe.c) emits the COM2 swarm-bridge framing, the DATA reply-body
geometry, the Lamport attestation parameters, the FSYN/FSYP coupling frame
layouts, and packed attestation-string known-answer entries into a .abi_ents
section under the REAL user build flags; we read it back with `objcopy -O
binary` (the extract-abi.py unpacker, verbatim). The HOST ring is imported
live from scripts/qos_bridge.py — the ONE host implementation of the same
wire — so the golden diff catches EITHER side drifting, and the twin
cross-check catches the two sides disagreeing with each other even when both
moved together away from the golden.

Modes:
  extract OBJ...                    print the canonical table to stdout
  emit --out GOLDEN OBJ...          write the canonical table to GOLDEN
  check --golden GOLDEN OBJ...      diff extracted vs GOLDEN

Exit-code discipline (the CI selftest asserts rc == 1, not merely != 0):
  1  contract signal ONLY: a golden diff, a guest/host TWIN MISMATCH, or a
     MUST-TWIN name missing from either ring.
  2  operational failure: missing golden, objcopy/probe trouble, a qos_bridge
     import error, or a mapped host attribute gone missing. An operational
     failure must never be mistakable for (or masked by) a contract diff.

QOS_WIRE_TEETH=1 (teeth live in THIS extractor, never in production code):
perturbs the host ring's SWARM_MAGIC by +1 in-memory, which MUST surface as a
TWIN MISMATCH (rc 1) — proving the twin cross-check actually bites. emit
REFUSES to run under teeth (rc 2), so a poisoned golden cannot be written.

`check` strips leading '# ' header-comment lines from the golden before the
strict text compare, so the committed file can carry its own regen/semver
instructions without them being contract bytes.

Stdlib-only by contract (the CI integration lane forbids pip).
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

REC = 64      # bytes per record
NAMELEN = 56  # NUL-padded ASCII name; the remaining 8 bytes are the LE u64 value

GOLDEN_HEADER = """\
# QuantumOS v1 WIRE contract (ADR-0020 lane C): COM2 swarm-bridge framing,
# reply-auth geometry, Lamport attestation parameters, FSYN/FSYP coupling
# frames, and attestation-string KATs — guest ring (user/wire_probe.c,
# compiler-measured) twinned against the host ring (scripts/qos_bridge.py).
# Regenerate ONLY for an INTENDED wire change (human-only, never in CI):
#   make regen-wire-golden
# then commit the diff. A v1 wire change is a contract break: follow the
# semver procedure in docs/adr/0020-v1-contract-freeze.md (bump the frozen
# contract version; a silent regen without the version call is a break).
"""

# The wire names that MUST exist on BOTH rings (logical names, ring prefix
# stripped). A refactor that drops one side's constant would otherwise leave a
# HOLE the plain diff reports only as a removed line on one ring — this makes
# it an explicit MUST-TWIN failure instead.
MUST_TWIN = frozenset([
    "wire:SWARM_MAGIC",
    "wire:SWARM_MAX_PAYLOAD",
    "wire:SWARM_HDR_LEN",
    "wire:FRAME_HANDSHAKE",
    "wire:FRAME_DATA",
    "wire:FRAME_PING",
    "wire:FRAME_PONG",
    "wire:FRAME_DISCONNECT",
    "wire:FRAME_PKDIGEST",
    "wire:FRAME_ATTEST",
    "wire:FRAME_SIG",
    "wire:SWARM_OP_STATUS",
    "wire:SWARM_OP_RECALL",
    "wire:SWARM_OP_QSUBMIT",
    "wire:SWARM_OP_KEY",
    "wire:LAMPORT_BITS",
    "wire:LAMPORT_HASH_LEN",
    "wire:LAMPORT_SIG_ELEM",
    "wire:LAMPORT_SIG_LEN",
    "wire:SWARM_CRC8_POLY",
    "wire:SWARM_CRC8_INIT",
    "wire:SWARM_REPLYAUTH_NONCE_LEN",
    "wire:SWARM_REPLYAUTH_TAG_LEN",
    "wire:SWARM_KEY_LEN",
    "wire:SWARM_STATUS_BODY_LEN",
    "wire:SWARM_RECALL_BODY_LEN",
    "wire:SWARM_QSUBMIT_BODY_ERR",
    "wire:SWARM_QSUBMIT_BODY_OK",
    "wire:SWARM_STATUS_R_SCALE",
    "kat:WIRE_ATTEST_HEAD_0",
    "kat:WIRE_ATTEST_HEAD_1",
    "kat:WIRE_ATTEST_TICKS_0",
])

# qos_bridge attribute -> frozen contract name, for host constants whose
# python spelling differs from the guest macro. Every mapped attribute MUST
# exist (a missing one is an operational failure, rc 2 — the extractor is
# broken, not the contract).
ALIAS = {
    "MAGIC": "SWARM_MAGIC",
    "SWARM_MAX_PAYLOAD": "SWARM_MAX_PAYLOAD",
    "HDR_LEN": "SWARM_HDR_LEN",
    "CRC8_POLY": "SWARM_CRC8_POLY",
    "CRC8_INIT": "SWARM_CRC8_INIT",
    "REPLYAUTH_NONCE_LEN": "SWARM_REPLYAUTH_NONCE_LEN",
    "REPLYAUTH_TAG_LEN": "SWARM_REPLYAUTH_TAG_LEN",
    "KEY_LEN": "SWARM_KEY_LEN",
    "STATUS_BODY_LEN": "SWARM_STATUS_BODY_LEN",
    "RECALL_BODY_LEN": "SWARM_RECALL_BODY_LEN",
    "QSUBMIT_BODY_ERR": "SWARM_QSUBMIT_BODY_ERR",
    "QSUBMIT_BODY_OK": "SWARM_QSUBMIT_BODY_OK",
    "STATUS_R_SCALE": "SWARM_STATUS_R_SCALE",
    "LAMPORT_BITS": "LAMPORT_BITS",
    "HASH_LEN": "LAMPORT_HASH_LEN",
    "SIG_ELEM": "LAMPORT_SIG_ELEM",
    "SIG_LEN": "LAMPORT_SIG_LEN",
}

# A host module constant matching this pattern joins the host ring even when
# unmapped: a NEW host wire constant then shows up as a golden '+' diff (a
# reviewable addition), never as an invisible hole.
AUTO_DISCOVER = re.compile(
    r"^(FRAME_|SWARM_OP_|LAMPORT_|CRC8_|REPLYAUTH_|STATUS_|RECALL_|QSUBMIT_"
    r"|KEY_LEN|HDR_LEN|MAGIC)")


def die(msg):
    """Operational failure: rc 2, never confusable with a contract diff."""
    sys.stderr.write(msg.rstrip("\n") + "\n")
    sys.exit(2)


def read_section(obj):
    """Return {name: unsigned_u64} for every record in obj's .abi_ents section."""
    fd, tmp = tempfile.mkstemp(suffix=".wiresec")
    os.close(fd)
    try:
        r = subprocess.run(
            ["objcopy", "-O", "binary", "--only-section=.abi_ents", obj, tmp],
            capture_output=True, text=True)
        if r.returncode != 0:
            die("objcopy failed on %s: %s" % (obj, r.stderr.strip()))
        with open(tmp, "rb") as f:
            raw = f.read()
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    if len(raw) == 0:
        die("%s: .abi_ents is empty (probe failed to compile?)" % obj)
    if len(raw) % REC != 0:
        die("%s: .abi_ents is %d bytes, not a multiple of %d" % (obj, len(raw), REC))
    ents = {}
    for i in range(0, len(raw), REC):
        rec = raw[i:i + REC]
        name = rec[:NAMELEN].split(b"\x00", 1)[0].decode("ascii")
        (value,) = struct.unpack("<Q", rec[NAMELEN:REC])
        if name in ents and ents[name] != value:
            die("%s: duplicate name %s with differing values" % (obj, name))
        ents[name] = value
    return ents


def pack8(data, off):
    """8 bytes of `data` starting at `off` (NUL-padded past the end) as LE u64
    — the python mirror of the probe's PACK8 macro."""
    window = bytes(data[off:off + 8]) + b"\x00" * 8
    return int.from_bytes(window[:8], "little")


def host_ring():
    """Build the host ring from scripts/qos_bridge.py (the ONE host wire
    implementation). Import/attribute trouble is operational (rc 2)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import qos_bridge
    except Exception as exc:  # noqa: BLE001 — any import failure is operational
        die("cannot import qos_bridge for the host ring: %s" % exc)

    ents = {}
    # Auto-discovered integer constants: a new host wire constant becomes a
    # reviewable golden '+' diff, not a hole.
    for attr in dir(qos_bridge):
        v = getattr(qos_bridge, attr)
        if AUTO_DISCOVER.match(attr) and isinstance(v, int) and not isinstance(v, bool):
            ents["host:wire:" + ALIAS.get(attr, attr)] = v & ((1 << 64) - 1)
    # Mapped constants: every ALIAS name must resolve.
    for attr, contract in ALIAS.items():
        if not hasattr(qos_bridge, attr):
            die("qos_bridge is missing mapped wire attribute %s (-> %s)" % (attr, contract))
        v = getattr(qos_bridge, attr)
        if not isinstance(v, int) or isinstance(v, bool):
            die("qos_bridge.%s is not an int (host ring broken)" % attr)
        ents["host:wire:" + contract] = v & ((1 << 64) - 1)

    # Attestation KATs, REBUILT the way parse_attestation consumes the string:
    # head = ATTEST_MAGIC + "|" + ATTEST_QSEED_KEY must byte-equal the guest's
    # SWARM_ATTEST_HEAD "QOS-BOOT|qseed=". Packing the reconstruction (never a
    # re-typed literal) is what twins the host PARSER against the guest EMITTER.
    for attr in ("ATTEST_MAGIC", "ATTEST_QSEED_KEY", "ATTEST_TICKS_KEY"):
        if not hasattr(qos_bridge, attr):
            die("qos_bridge is missing attestation piece %s" % attr)
    head = qos_bridge.ATTEST_MAGIC + "|" + qos_bridge.ATTEST_QSEED_KEY
    ticks = "|" + qos_bridge.ATTEST_TICKS_KEY
    if len(head) > 15 or len(ticks) > 7:
        die("attestation piece grew past its KAT window (head %d > 15 or "
            "ticks %d > 7) — extend the KAT entries on BOTH rings" % (len(head), len(ticks)))
    head_b = head.encode("ascii")
    ents["host:kat:WIRE_ATTEST_HEAD_0"] = pack8(head_b, 0)
    ents["host:kat:WIRE_ATTEST_HEAD_1"] = pack8(head_b, 8)
    ents["host:kat:WIRE_ATTEST_TICKS_0"] = pack8(ticks.encode("ascii"), 0)

    # Behavioural KAT, host-only (documented UNTWINNED: the guest builds
    # frames imperatively, so there is no guest expression to twin against):
    # the exact 5 wire bytes of an empty PING — magic+type+len exercise the
    # header layout and the CRC8 parameters end to end through frame()/crc8().
    ents["host:kat:ping_frame"] = int.from_bytes(
        qos_bridge.frame(qos_bridge.FRAME_PING, b""), "little")

    if os.environ.get("QOS_WIRE_TEETH") == "1":
        # Teeth live HERE, in the extractor, never in qos_bridge itself: a
        # production-code hook could be shipped enabled. Perturbing a TWINNED
        # value exercises the TWIN MISMATCH path specifically.
        ents["host:wire:SWARM_MAGIC"] += 1
    return ents


def collect(objs):
    merged = host_ring()
    for o in objs:
        for name, value in read_section(o).items():
            if name in merged and merged[name] != value:
                die("duplicate name %s across rings with differing values" % name)
            merged[name] = value
    if not merged:
        die("no wire entries extracted from %r" % (objs,))
    return merged


def render(v):
    """u64 -> stable decimal; values with the sign bit set render as int64 so
    errnos read as -1, -11 rather than a giant unsigned constant."""
    return str(v - (1 << 64)) if v >= (1 << 63) else str(v)


def canonical(ents):
    return "".join("%s = %s\n" % (n, render(ents[n])) for n in sorted(ents))


def strip_header(text):
    """Drop the leading '#' comment block (the emit-time header) so the strict
    compare sees only contract lines."""
    lines = text.split("\n")
    i = 0
    while i < len(lines) and lines[i].startswith("#"):
        i += 1
    return "\n".join(lines[i:])


def twin_errors(ents):
    """A logical name (ring prefix stripped) present on both rings must carry
    the same value."""
    by_logical = {}
    for name, v in ents.items():
        ring, rest = name.split(":", 1)
        by_logical.setdefault(rest, {})[ring] = v
    errs = []
    for rest, rings in sorted(by_logical.items()):
        if "guest" in rings and "host" in rings and rings["guest"] != rings["host"]:
            errs.append("TWIN MISMATCH %s: guest=%s host=%s"
                        % (rest, render(rings["guest"]), render(rings["host"])))
    return errs


def must_twin_errors(ents):
    """Every MUST_TWIN logical name must exist on BOTH rings — a one-sided
    deletion is a contract hole, not a benign diff."""
    have = {}
    for name in ents:
        ring, rest = name.split(":", 1)
        have.setdefault(rest, set()).add(ring)
    errs = []
    for rest in sorted(MUST_TWIN):
        rings = have.get(rest, set())
        for ring in ("guest", "host"):
            if ring not in rings:
                errs.append("MUST-TWIN MISSING %s: no %s-ring entry" % (rest, ring))
    return errs


def main():
    ap = argparse.ArgumentParser(description="QuantumOS v1 WIRE freeze gate (ADR-0020 lane C)")
    ap.add_argument("mode", choices=["extract", "emit", "check"])
    ap.add_argument("objs", nargs="+", help="compiled wire-probe object files")
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
        if os.environ.get("QOS_WIRE_TEETH"):
            # Refuse to mint a golden from a teeth-perturbed ring: an emit run
            # under the selftest environment would freeze the mutation.
            die("refusing to emit a golden with QOS_WIRE_TEETH set")
        if terrs:
            sys.stderr.write("refusing to emit a golden with twin mismatches:\n")
            sys.stderr.write("\n".join(terrs) + "\n")
            return 1
        outdir = os.path.dirname(args.out)
        if outdir:
            os.makedirs(outdir, exist_ok=True)
        with open(args.out, "w", newline="\n") as f:
            f.write(GOLDEN_HEADER)
            f.write(text)
        sys.stderr.write("wrote %d entries to %s\n" % (len(ents), args.out))
        return 0

    # check
    if not args.golden:
        ap.error("check needs --golden")
    if not os.path.exists(args.golden):
        die("FROZEN WIRE GATE: golden %s is missing -- run "
            "`make regen-wire-golden` (operational, not a diff)" % args.golden)
    with open(args.golden, "r", newline="") as f:
        want = strip_header(f.read().replace("\r\n", "\n"))
    rc = 0
    mterrs = must_twin_errors(ents)
    if mterrs:
        sys.stderr.write("FROZEN WIRE GATE: must-twin names missing:\n")
        sys.stderr.write("\n".join(mterrs) + "\n")
        rc = 1
    if terrs:
        sys.stderr.write("FROZEN WIRE GATE: guest/host twins disagree:\n")
        sys.stderr.write("\n".join(terrs) + "\n")
        rc = 1
    if text != want:
        rc = 1
        sys.stderr.write("FROZEN WIRE GATE: extracted wire contract != %s\n" % args.golden)
        want_lines = set(want.splitlines())
        got_lines = set(text.splitlines())
        for line in sorted(got_lines - want_lines):
            sys.stderr.write("  + %s\n" % line)
        for line in sorted(want_lines - got_lines):
            sys.stderr.write("  - %s\n" % line)
        sys.stderr.write("If this change is intended, run `make regen-wire-golden` "
                         "and commit the golden diff.\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
