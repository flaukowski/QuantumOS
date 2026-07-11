# 15. The Agent-Native Host Surface

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

The north star is that agents *operate* the OS. Inside the guest, authority is
structural (ADR-0001, ADR-0008). But an agent runs on the host, outside the VM, and
needs a surface to drive a running QuantumOS through: boot it, script its shell,
imprint and recall from its field, run programs, reach the network, read the audit
ledger — and, critically, *know it is talking to the machine it thinks it is*. That
last requirement is what separates this from a generic QEMU wrapper: every result
must carry a verifiable identity, and the surface must be honest about the limits of
that verification.

## Decision

The host surface is an **MCP server** (`scripts/qos_mcp.py`, 21 `@mcp.tool`
functions) that are one-line delegations to a **stdlib-only bridge**
(`scripts/qos_bridge.py`). The bridge is stdlib-only *by contract* — it must never
`import mcp`, enforced by a test that asserts `mcp` is absent from `sys.modules`
(scripts/test_qos_mcp.py) — so the guest-facing plumbing has zero third-party
dependency and the `mcp` package serves only the tool layer.

The transport split mirrors the trust story: **COM1 carries qsh scripting** (drained
on a thread) and **COM2 carries attested data** — QEMU connects *out* to a host
listener so byte 0 of the boot attestation is captured with no port race
(scripts/qos_bridge.py:586-624). Every tool result carries the **Lamport-verified
boot identity** (ADR under the ghostOS phase-4 attestation), and the server is
explicit that *verified ≠ live*: the signature covers the boot attestation string,
not the liveness of any later reply (scripts/qos_mcp.py:18-23). Errors are returned
as `{error, identity}` dicts, never raised, so an agent always gets a structured,
attributable failure.

`qsh` input is validated as a **security boundary**: printable ASCII only, a
119-byte line budget, and nine reserved markers (`qsh:`, `FIELD:`, `AUDIT:`, …)
refused so a scripted line cannot forge a kernel-anchored output line
(scripts/qos_bridge.py:324-357). The **Kannaka memory bridge** shells out to a
configured binary under a `KANNAKA_READONLY` discipline (reads force it, writes strip
it) and refuses argv that could be misread as flags.

## Consequences

### Positive
- An agent operates the OS through a typed tool surface where every answer is tied
  to a verified boot identity — the concrete realization of "agents operate the OS".
- The stdlib-only bridge contract keeps the guest-driving path dependency-free and
  makes the `mcp` boundary testable, not aspirational.
- The COM1/COM2 split gives scripting and attestation independent channels; the
  connect-out design removes a TOCTOU on the attestation capture.
- qsh input validation closes output-line forgery at the host boundary, complementing
  the kernel's per-line prefixing.

### Negative
- **Host-side ambient authority**: the MCP server grants every connected client all
  22 tools — boot, shutdown, fs-write, fetch, qpu-submit — with no host-side manifest or quota
  analog to the guest's intent model (ADR-0008). Kernel safety is structural; host
  safety is not. This is a named trust boundary, and a per-session tool allowlist
  mirroring the guest manifest is the natural follow-on to ADR-0019.
- The quantum broker is now on the surface: `qos_qpu_submit` (the 22nd tool) lets an
  agent run a capability-gated, quota-metered, ledgered circuit (ADR-0013) — this gap
  is closed.
- Documented drift: `qos_memory_import`'s docstring claims strength is not carried
  into slot energy, but the bridge maps similarity → `--energy` and CI asserts it
  (scripts/qos_mcp.py:211-212 vs qos_bridge.py:1161-1177). A pre-freeze fix (ADR-0020).
- The Kannaka bridge default binary path is Windows-specific
  (scripts/qos_bridge.py:435-437) — portable via env var but a poor default.
- Verified ≠ liveness: a replayed COM2 DATA reply is not caught by the boot
  signature; authenticating the reply frames is proposed alongside ADR-0019.

### Residual risks
- The whole surface trusts the host process boundary — anyone who can reach the MCP
  server has full authority over the VM. Single-tenant today; a shared deployment
  needs the host trust boundary named above before exposure.
- All documentation targets humans, but the surface's user is an agent — a
  machine-readable tool contract shipped in the package (ADR-0018/0020) is the
  missing piece for an LLM to operate the OS from docs alone.

## Evidence
- Shipped in: PR #119 — MCP server exposing a running QuantumOS as MCP tools (epic #99)
- Shipped in: PR #126 — deepened toolbox: run-args, sysinfo, qrand, fs, fetch (epic #125)
- Shipped in: PR #128 — Kannaka HRM memory bridge (epic #127)
- Shipped in: PR #130 — read-only SYS_FIELD_INFO + imprint --energy
- Shipped in: PR #132 / #141 — two-VM and N-way society tools (ADR-0014)
- Key code: scripts/qos_mcp.py (22 tools, :18-23 verified≠live, :38-39 error dicts,
  :211-212 docstring drift); scripts/qos_bridge.py:586-624 (COM2 connect-out),
  :324-357 (qsh input validation), :432-507 (Kannaka bridge)
- CI gates: `ci-smoke-mcp` (scripts/test_qos_mcp.py — every tool, identity on each
  result, the `mcp`-not-imported assertion, reserved-marker forgery refusal)
