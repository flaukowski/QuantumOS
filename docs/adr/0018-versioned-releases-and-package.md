# 18. Versioned Releases and a Published Host Package

Date: 2026-07-11
Status: Accepted

## Context

QuantumOS has 126 merged PRs (#7–#183) and has **never cut a tagged release**. The
version string `QuantumOS v0.1` is hard-coded in one place in code
(kernel/src/main.c:90) and echoed by several docs; there is no `VERSION` file, no
`CHANGELOG.md`, and no tag. The CI `release` job builds `kernel.elf` and `kernel.iso`
but only uploads them as 90-day artifacts (.github/workflows/ci.yml:975-1021) — they
expire, and nothing is ever published to users. The agent-native host surface
(ADR-0015) *is the product*, yet it is uninstallable: there is no `pyproject.toml`,
`setup.py`, or `requirements` anywhere, and the modules self-locate via
`sys.path.insert`. A project this substantial with no release and no installable
package is under-delivering on its own work.

## Decision

Ship the first tagged release and a published host package.

- **Single-source the version.** A root `VERSION` file holds `0.4.0`. The kernel
  banner reads it via a build define (`-DQOS_VERSION`), replacing the hard-coded
  string. Docs reference the file, not a literal.
- **Milestone semver, honestly retrospective.** `v0.4.0` is the **first tagged
  release**, at the current head. `v0.1` (bootstrap) / `v0.2` (interactive OS on real
  hardware) / `v0.3` (field memory + agent surface + societies) are recorded as
  retrospective `CHANGELOG.md` milestones — *not* retro-tagged on historical commits.
  The changelog's backbone is the 22 PR arcs.
- **Tag-triggered release workflow.** A push of `vX.Y.Z` (matched against `VERSION`)
  builds the kernel images, the 4-entry GRUB ISO, a self-contained web-demo bundle,
  and the Python sdist/wheel; **boot-verifies the exact ISO** (`ci-smoke-iso`) and
  the banner version; and publishes a GitHub Release with notes extracted from the
  changelog section. `workflow_dispatch` runs everything as a dry run so the pipeline
  is testable before any tag exists.
- **Package the host toolkit** as `quantumos-host-tools` via `pyproject.toml`
  (package-dir `scripts/`, console entry points `qos-mcp` / `qos-verify-attestation`
  / `qos-qbraid-boot`, dynamic version from `VERSION`, dependency `mcp>=1.0`, extras
  for the quantum/qbraid backends). Publish at **0.x now** — semver imposes no
  compatibility promise below 1.0, so packaging carries no ossification risk. The v1
  *freeze* is deliberately a separate decision (ADR-0020), because it must wait for
  ADR-0019 to finish extending the COM2/attestation contracts.
- **Self-host the qemu-wasm binary** as a release asset, removing the upstream-CDN
  single point of failure under the live browser demo.
- **PyPI publish is prepared but gated** on an owner-configured `PYPI_API_TOKEN`
  secret: absent the token the job emits a notice and skips (the artifacts are on the
  GitHub Release regardless), so the path is honest and becomes active the moment
  Nick adds the secret.

## Consequences

### Positive
- Users get an installable ISO, kernel images, a reproducible browser bundle, and a
  `pip install`-able agent toolkit — the work becomes consumable.
- The release is boot-verified: a tag cannot publish an ISO that does not reach the
  shell, and the banner-version gate ties the artifact to the tag.
- Packaging at 0.x unblocks distribution today without freezing contracts that
  ADR-0019 still needs to change.

### Negative
- The host package pollutes the top-level module namespace (`qos_bridge`, `qos_mcp`,
  …) — acceptable at 0.x, revisited if it becomes a real collision risk.
- Retrospective milestones are a narrative convenience: 0.1–0.3 never existed as
  tags, and the changelog says so to avoid implying a release history that isn't there.
- `qsv_gateway` imports PennyLane at module top, so the wheel's import smoke-test must
  avoid importing it without the `quantum` extra — a packaging sharp edge to verify.

### Residual risks
- The release workflow runs its own `ci-smoke-iso` rather than trusting the ci.yml
  run on the same commit, because a tag may be pushed on a commit whose CI ran days
  earlier — at the cost of a second full ISO build per release.
- PyPI name availability for `quantumos-host-tools` is unverified (no network at
  authoring time); a collision would force a rename before the first PyPI publish.

## Evidence
- Being implemented: this session (branch `feat/release-engineering`) — `VERSION`,
  `CHANGELOG.md`, `pyproject.toml`, `.github/workflows/release.yml`, kernel banner
  single-sourcing, browser-demo path-filter fix
- Key code (as-built baseline): kernel/src/main.c:90 (hard-coded banner);
  .github/workflows/ci.yml:975-1021 (artifact-only release job); scripts/qos_mcp.py,
  scripts/qos_bridge.py (the package surface, ADR-0015); web/qemu/fetch-wasm.sh
  (SHA-pinned qemu-wasm to self-host)
- Cross-references: ADR-0015 (the surface being packaged), ADR-0019/0020 (why the v1
  freeze waits)
