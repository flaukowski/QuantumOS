#!/usr/bin/env python3
"""
QuantumOS MCP server (epic #99): expose a running QuantumOS to agents as
tools. Any MCP-speaking agent can boot a VM, query its ghostd field, imprint
and recall associative memories at the kernel field, run a citizen off the
initrd, and shut it down — every result carrying the boot's Lamport-verified
identity.

This is a THIN wrapper: all logic (framing, attestation, the QEMU lifecycle,
the verified-gate) lives in qos_bridge.QosVM. The tools here are one-line
delegations, so the CI test in test_qos_mcp.py exercises the very same code
path these tools take. Only this file imports `mcp`; qos_bridge stays
stdlib-only so the integration CI (no mcp package) can test the bridge.

HONESTY: `identity()` in every result documents that `verified` means the
boot attestation's frames + Lamport signature + qseed are consistent — NOT
that the VM is live-now (a captured attestation replays) and NOT that a
STATUS/RECALL reply is individually authenticated (DATA traffic is unsigned).
Trust equals control of the VM's serial channel, the same class as holding
the console. Do not present tool results as cryptographically attested data.

Run:  python3 scripts/qos_mcp.py       (stdio transport)
Dep:  mcp>=1.0  (Python >=3.10)
"""

from mcp.server.fastmcp import FastMCP

from qos_bridge import QosVM, QosError

mcp = FastMCP("quantumos")
_vm = QosVM()


def _err(exc):
    return {"error": str(exc), "identity": _vm.identity()}


@mcp.tool()
def qos_boot(qseed: str = "", kernel: str = "") -> dict:
    """Boot a QuantumOS VM under QEMU and capture its Lamport boot attestation.

    qseed: optional hex quantum seed folded into the attested identity (the
    boot is deterministic under a fixed qseed). kernel: optional path to a
    kernel.elf32 (defaults to the repo build). One VM at a time — call
    qos_shutdown first if one is already running. Returns the verified boot
    identity (see the module docstring for what 'verified' honestly means)."""
    global _vm
    if _vm.is_running():
        return {"error": "a VM is already running — call qos_shutdown first",
                "identity": _vm.identity()}
    _vm = QosVM(kernel=kernel or None)
    try:
        return {"booted": True, "identity": _vm.boot(qseed=qseed or None)}
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_status() -> dict:
    """Query ghostd's field over the attested COM2 bridge: the order parameter
    R and the count of live imprinted patterns. Refuses if the boot
    attestation is not verified."""
    try:
        return _vm.status()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_imprint(text: str) -> dict:
    """Imprint `text` into the kernel holographic field (the same associative
    memory a human reaches with the qsh `imprint` command). Returns the
    assigned slot. Refuses if the attestation is not verified."""
    try:
        return _vm.imprint(text)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_recall(text: str) -> dict:
    """Recall from the kernel holographic field with a (possibly corrupted)
    probe `text`; returns the winning stored text, its slot and score. Refuses
    if the attestation is not verified."""
    try:
        return _vm.recall(text)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_run(program: str) -> dict:
    """Spawn a citizen off the initrd (e.g. '/bin/hello') via qsh and return
    its console output and exit code. `program` must be /bin/<name>. Refuses
    if the attestation is not verified."""
    try:
        return _vm.run(program)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_shutdown() -> dict:
    """Power off the running VM (SIGTERM -> SIGKILL of QEMU; qsh `exit` only
    reboots the shell, never the machine). Idempotent."""
    return _vm.shutdown()


if __name__ == "__main__":
    mcp.run()
