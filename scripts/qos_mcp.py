#!/usr/bin/env python3
"""
QuantumOS MCP server (epics #99, #125): expose a running QuantumOS to agents
as tools. Any MCP-speaking agent can boot a VM; query its ghostd field; imprint
and recall associative memories at the kernel field; inspect that field
read-only (live slots, energy, eviction); run a citizen off the initrd (with
arguments); read a structured machine snapshot (uptime, memory, process, date); draw quantum-seeded entropy; write/rm/sync overlay files;
have the OS fetch a URL over its own TCP stack; bridge memories to/from the host
Kannaka HRM (epic #127); and shut it down — every result carrying the boot's
Lamport-verified identity.

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
def qos_field_status() -> dict:
    """Inspect the kernel holographic field READ-ONLY (epic #127 B1): live slot
    count, capacity, and per-slot metadata — slot index, length, energy (% of
    max, the stored importance), effective energy (% after age decay),
    retrievals, age, and a bounded preview. Unlike a recall this does NOT
    reinforce the field, so repeated calls do not perturb it. This is a real
    live count (not saturated) and content-addressable capacity/eviction
    visibility. Refuses if the attestation is not verified."""
    try:
        return _vm.field_info()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_run(program: str, args: str = "") -> dict:
    """Spawn a citizen off the initrd (e.g. '/bin/hello' or '/bin/args a b c')
    via qsh and return its console output and exit code. `program` must be
    /bin/<name>; `args` is an optional argument string (validated — no control
    bytes, bounded to the qsh line, no result-marker impersonation). Refuses if
    the attestation is not verified."""
    try:
        return _vm.run(program, args=args)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_sysinfo() -> dict:
    """One structured snapshot of the running machine: uptime (ticks + seconds),
    memory (heap-free bytes, free/total frames), the live process table
    (pid/name/state), and the RTC date. Refuses if not verified."""
    try:
        return _vm.sysinfo()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_qrand() -> dict:
    """Draw 64 bits of quantum-seeded entropy from the guest (SYS_QRAND,
    capability-gated in-kernel). Returns lowercase hex. Refuses if not
    verified."""
    try:
        return _vm.qrand()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_fetch(host: str, port: int = 80, allow_private: bool = False) -> dict:
    """Have QuantumOS fetch http://host:port/ over its own TCP stack; returns
    the status line, byte count, and whether the transfer completed.

    This reaches the HOST network namespace via SLIRP NAT (loopback, DNS, and
    outbound internet are live), NOT a sandbox — so loopback/RFC1918 targets
    are refused by default (SSRF guard); set allow_private=True to override.
    The response BODY is not returned (qsh's `http` prints only the status line
    and byte count). Refuses if the attestation is not verified."""
    try:
        return _vm.fetch(host, port=port, allow_private=allow_private)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_fs(op: str, path: str = "", text: str = "") -> dict:
    """Overlay-filesystem operation over qsh. op is 'write' (path + text),
    'rm' (path), or 'sync' (flush the RAM overlay to disk). Paths and text are
    validated before they reach the qsh line. read/ls are not offered yet —
    their console output is not machine-framed (a filed guest follow-up).
    Refuses if the attestation is not verified."""
    try:
        return _vm.fs(op, path=path, text=text)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_memory_import(query: str, top_k: int = 5) -> dict:
    """Recall the top-k memories for `query` from the host Kannaka HRM and
    imprint each into the QuantumOS kernel field — seeding the OS's associative
    memory from the host memory system. Content is truncated to the 64-byte
    field slot (reported) and non-ASCII memories are skipped (the field is
    ASCII-only); Kannaka strength is NOT carried into slot energy. Requires
    kannaka.exe (QOS_KANNAKA_BIN). Refuses if the attestation is not verified."""
    try:
        return _vm.memory_import(query, top_k=top_k)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_memory_export(probe: str, importance: float = 0.6,
                      allow_write: bool = False) -> dict:
    """Recall the field completion for `probe` and persist it to the host
    Kannaka HRM. REFUSES unless allow_write=True: this WRITES the user's real
    ~/.kannaka memory (or KANNAKA_DATA_DIR) with field-derived content, and it
    REINFORCES the recalled field slot (not a pure read). `importance` is a
    caller constant. Refuses if the attestation is not verified."""
    try:
        return _vm.memory_export(probe, importance=importance, allow_write=allow_write)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_memory_bridged_recall(query: str, top_k: int = 3) -> dict:
    """Query BOTH memory systems on the same cue: the QuantumOS field completion
    (resonance = cosine x energy) and the Kannaka HRM's top-k. The field recall
    REINFORCES its winner, and field_score_q15 is cosine x energy in Q15 (NOT a
    bare similarity). Requires kannaka.exe. Refuses if not verified."""
    try:
        return _vm.bridged_recall(query, top_k=top_k)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_shutdown() -> dict:
    """Power off the running VM (SIGTERM -> SIGKILL of QEMU; qsh `exit` only
    reboots the shell, never the machine). Idempotent."""
    return _vm.shutdown()


if __name__ == "__main__":
    mcp.run()
