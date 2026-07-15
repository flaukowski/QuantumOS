#!/usr/bin/env python3
"""Extract + freeze the QuantumOS v1 MCP tool surface (ADR-0020 lane B).

Imports scripts/qos_mcp.py (the FastMCP server), lists its tools, and either
emits or checks the committed golden contracts/mcp/v1-tools.json. What is
FROZEN is each tool's `name` + normalized `inputSchema` — the surface an
agent's tool-call marshalling depends on. Docstring-derived `title` and
`description` keys are STRIPPED recursively before the compare (pydantic
re-words those without any wire consequence), and tool RESULT-dict shapes are
documented-not-gated (they are dynamic dicts; the machine-readable agent-docs
item owns them).

The golden is generator-version-pinned: pydantic/mcp derive JSON schemas, and
a resolver bump can re-shape a schema with no source change. check therefore
self-verifies importlib.metadata versions against the golden's _meta pins
FIRST — a mismatch is 'GENERATOR SKEW', exit 2 (operational: fix the
environment via requirements-mcp-gate.txt, or intentionally re-pin), never a
fake contract diff.

Exit-code discipline (the CI selftest asserts rc == 1, not merely != 0):
  1  contract diff ONLY (tool added/removed/schema drift vs the golden)
  2  operational (missing/unparseable golden, import failure, version skew)

QOS_MCP_TEETH (teeth live in THIS extractor, never in qos_mcp.py):
  del  delete one property from the qos_qpu_submit inputSchema in-memory —
       a changed-VALUE mutation (the tool LIST is unchanged), proving the
       gate catches more than an added/removed name.
  add  append a bogus tool.
emit REFUSES to run under teeth (exit 2), so a poisoned golden cannot be
written. Modes: extract | emit --out GOLDEN | check --golden GOLDEN.

SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import asyncio
import importlib.metadata
import json
import os
import sys

CONTRACT = "qos-mcp-tools"
CONTRACT_VERSION = 1
REGEN = "make regen-mcp-golden"
FROZEN = ("name+inputSchema; descriptions and result-dict shapes excluded "
          "(documented in the machine-readable agent docs, not gated)")
# The schema GENERATORS whose versions are pinned in requirements-mcp-gate.txt
# and self-checked against the golden's _meta on every run.
PINNED_DISTS = ("mcp", "pydantic", "pydantic-core")

BOGUS_TOOL_NAME = "qos_teeth_bogus_tool"


def die(msg):
    """Operational failure: rc 2, never confusable with a contract diff."""
    sys.stderr.write(msg.rstrip("\n") + "\n")
    sys.exit(2)


def strip_meta(node):
    """Recursively drop 'title' and 'description' keys: pydantic derives them
    from docstrings/field names, which are documentation, not wire contract."""
    if isinstance(node, dict):
        return {k: strip_meta(v) for k, v in node.items()
                if k not in ("title", "description")}
    if isinstance(node, list):
        return [strip_meta(v) for v in node]
    return node


def measured_versions():
    out = {}
    for dist in PINNED_DISTS:
        try:
            out[dist] = importlib.metadata.version(dist)
        except importlib.metadata.PackageNotFoundError:
            die("generator package %r is not installed — pip install -r "
                "requirements-mcp-gate.txt (or use the make venv)" % dist)
    return out


def load_tools():
    """Import qos_mcp and return the sorted frozen-surface entries, with the
    QOS_MCP_TEETH mutation applied in-memory when requested."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import qos_mcp
    except Exception as exc:  # noqa: BLE001 — any import failure is operational
        die("cannot import qos_mcp (mcp package missing?): %s" % exc)
    try:
        tools = asyncio.run(qos_mcp.mcp.list_tools())
    except Exception as exc:  # noqa: BLE001
        die("mcp.list_tools() failed: %s" % exc)
    entries = [{"name": t.name, "inputSchema": strip_meta(t.inputSchema)}
               for t in tools]
    if not entries:
        die("qos_mcp exposes zero tools (extractor or server broken)")

    teeth = os.environ.get("QOS_MCP_TEETH")
    if teeth == "del":
        for e in entries:
            if e["name"] == "qos_qpu_submit":
                props = e["inputSchema"].get("properties") or {}
                if not props:
                    die("teeth=del: qos_qpu_submit has no properties to delete")
                victim = "probe" if "probe" in props else sorted(props)[0]
                del props[victim]
                break
        else:
            die("teeth=del: qos_qpu_submit tool not found")
    elif teeth == "add":
        entries.append({"name": BOGUS_TOOL_NAME,
                        "inputSchema": {"properties": {}, "type": "object"}})
    elif teeth:
        die("unknown QOS_MCP_TEETH mode %r (want del|add)" % teeth)

    entries.sort(key=lambda e: e["name"])
    return entries


def render(entries, versions):
    doc = {
        "_meta": {
            "contract": CONTRACT,
            "version": CONTRACT_VERSION,
            "mcp": versions["mcp"],
            "pydantic": versions["pydantic"],
            "pydantic-core": versions["pydantic-core"],
            "regen": REGEN,
            "frozen": FROZEN,
        },
        "tools": entries,
    }
    return json.dumps(doc, sort_keys=True, indent=2) + "\n"


def tool_map(doc):
    return {t["name"]: t.get("inputSchema") for t in doc.get("tools", [])}


def main():
    ap = argparse.ArgumentParser(
        description="QuantumOS v1 MCP tool-surface freeze gate (ADR-0020 lane B)")
    ap.add_argument("mode", choices=["extract", "emit", "check"])
    ap.add_argument("--golden", help="committed golden file (check mode)")
    ap.add_argument("--out", help="golden file to write (emit mode)")
    args = ap.parse_args()

    versions = measured_versions()

    if args.mode == "emit":
        if not args.out:
            ap.error("emit needs --out")
        if os.environ.get("QOS_MCP_TEETH"):
            # Refuse to mint a golden from a teeth-mutated surface: an emit
            # run under the selftest environment would freeze the mutation.
            die("refusing to emit a golden with QOS_MCP_TEETH set")
        entries = load_tools()
        outdir = os.path.dirname(args.out)
        if outdir:
            os.makedirs(outdir, exist_ok=True)
        with open(args.out, "w", newline="\n") as f:
            f.write(render(entries, versions))
        sys.stderr.write("wrote %d tools to %s (mcp %s, pydantic %s)\n"
                         % (len(entries), args.out, versions["mcp"], versions["pydantic"]))
        return 0

    entries = load_tools()
    text = render(entries, versions)

    if args.mode == "extract":
        sys.stdout.write(text)
        return 0

    # check
    if not args.golden:
        ap.error("check needs --golden")
    if not os.path.exists(args.golden):
        die("FROZEN MCP GATE: golden %s is missing -- run `%s` "
            "(operational, not a diff)" % (args.golden, REGEN))
    with open(args.golden, "r", newline="") as f:
        want = f.read().replace("\r\n", "\n")
    try:
        want_doc = json.loads(want)
    except json.JSONDecodeError as exc:
        die("FROZEN MCP GATE: golden %s is not valid JSON: %s" % (args.golden, exc))

    # Generator self-check FIRST: a pydantic/mcp resolver bump re-shapes
    # schemas with no source change, which must surface as SKEW (rc 2, fix the
    # pin), never as a contract diff the teeth selftest could mistake for a
    # caught mutation.
    meta = want_doc.get("_meta", {})
    for dist in PINNED_DISTS:
        pinned = meta.get(dist)
        if pinned != versions[dist]:
            die("FROZEN MCP GATE: GENERATOR SKEW: %s golden pins %r but this "
                "environment has %r — align requirements-mcp-gate.txt / the CI "
                "pip install with the golden's _meta (or intentionally re-pin "
                "via `%s`)" % (dist, pinned, versions[dist], REGEN))

    if text == want:
        return 0

    # Structured per-tool report (named markers), then the strict verdict.
    sys.stderr.write("FROZEN MCP GATE: extracted tool surface != %s\n" % args.golden)
    got_tools = tool_map({"tools": entries})
    want_tools = tool_map(want_doc)
    for name in sorted(set(got_tools) - set(want_tools)):
        sys.stderr.write("  TOOL ADDED: %s\n" % name)
    for name in sorted(set(want_tools) - set(got_tools)):
        sys.stderr.write("  TOOL REMOVED: %s\n" % name)
    for name in sorted(set(got_tools) & set(want_tools)):
        if got_tools[name] != want_tools[name]:
            sys.stderr.write("  TOOL SCHEMA DRIFT: %s\n" % name)
    want_lines = set(want.splitlines())
    got_lines = set(text.splitlines())
    for line in sorted(got_lines - want_lines):
        sys.stderr.write("  + %s\n" % line)
    for line in sorted(want_lines - got_lines):
        sys.stderr.write("  - %s\n" % line)
    sys.stderr.write("If this change is intended, run `%s` and commit the "
                     "golden diff.\n" % REGEN)
    return 1


if __name__ == "__main__":
    sys.exit(main())
