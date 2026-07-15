#!/usr/bin/env python3
"""Claude ↔ QuantumOS: a bring-your-own-key agent that drives a live QuantumOS
VM through its FROZEN MCP tool surface (ADR-0020 lane B, contracts/mcp/
v1-tools.json). Claude is exactly the external consumer that freeze was built
for — so this script REUSES scripts/qos_mcp.py verbatim rather than duplicating
a single tool definition. One source of truth: the same 22 tools CI diffs
against the golden are the tools Claude sees.

Architecture (all local, no hosted anything):

    Claude (Anthropic API, your key)
        │  tool_runner drives the agentic loop
        ▼
    anthropic MCP conversion helpers  (async_mcp_tool)
        │  stdio
        ▼
    scripts/qos_mcp.py  (FastMCP, unmodified)
        │  thin delegation
        ▼
    qos_bridge.QosVM / QosSociety  →  QEMU  →  QuantumOS

The key never leaves your machine; the VM never leaves your machine. Claude
plans and runs field-society / associative-memory experiments over the wire,
reads the results, and iterates.

HONESTY (mirrors qos_mcp.py's module docstring): a tool result's `verified`
means the boot attestation is self-consistent (frames + Lamport signature +
qseed), NOT that the VM is live-now — a captured attestation replays. Only the
key-admitted STATUS reply is per-request nonce+HMAC fresh. The system prompt
tells Claude this so it never overstates what a result proves.

Run:
    # 1. build the guest once (produces build/x86_64/kernel.elf32)
    make kernel
    # 2. bring your own Anthropic credentials — either works:
    export ANTHROPIC_API_KEY=sk-ant-...        # or: ant auth login
    # 3. install deps and run
    pip install -r requirements-claude-agent.txt
    python3 scripts/qos_claude_agent.py "Boot a VM and imprint three phrases, then recall a corrupted probe of the first."

    # prove the MCP handshake without spending a token / needing a key:
    python3 scripts/qos_claude_agent.py --list-tools

Deps: anthropic[mcp]>=0.116.0, mcp>=1.0  (Python >=3.10)
"""

import argparse
import asyncio
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Default experiment when none is given on the command line. Deliberately
# open-ended: Claude designs the run rather than following a script (Fable-5
# does better with a goal than a recipe).
DEFAULT_TASK = (
    "You are driving a live QuantumOS instance. Design and run a small "
    "associative-memory experiment: boot a VM, imprint three distinct short "
    "phrases into the kernel Hopfield-Kuramoto field, then recall a corrupted "
    "probe of one of them and report whether the field relaxed to the right "
    "basin and with what order parameter R. Draw one quantum-seeded number to "
    "salt the run. Shut the VM down when finished, and give me a plain-language "
    "readout of what happened and what it demonstrates about the field."
)

SYSTEM = """You are an experimentalist driving a live QuantumOS instance — a \
capability-secured microkernel whose kernel hosts a Hopfield–Kuramoto \
associative-memory field ("ghostd"), quantum-seeded entropy, and multi-node \
"field societies" that couple over the wire. You act only through the provided \
QuantumOS tools; there is no shell.

What you can do: boot and shut down VMs; imprint and recall associative \
memories at the kernel field and read that field's live state; form N-node \
societies (3–4 members) that mean-field-synchronize and exchange qseed-salted \
aggregates; draw quantum-seeded entropy; run citizens off the initrd; read \
machine snapshots; and bridge memories to/from the host Kannaka HRM.

Method: state your hypothesis, run the smallest experiment that tests it, read \
the actual tool results (do not assume — a boot can fail, a recall can miss), \
and iterate. Prefer one clear result over an exhaustive sweep.

Honesty about trust — this matters: a result's `verified` flag means the boot \
attestation is internally consistent (frames + Lamport signature + qseed), NOT \
that the VM is live at read time; a captured attestation replays. Only a \
key-admitted STATUS reply is per-request authenticated. Never describe an \
ordinary tool result as cryptographically attested data. Report what the field \
did, plainly, and distinguish measured from inferred.

Operational: booting a VM or a society takes tens of seconds to a few minutes \
— that is normal, not a hang. Always shut down what you booted before ending, \
even on failure. When done, give a plain-language readout: what you ran, what \
the field did, and what it demonstrates."""


async def _open_tools(mcp_client_ctx):
    """List the QuantumOS MCP tools from an initialized ClientSession."""
    result = await mcp_client_ctx.list_tools()
    return result.tools


def _kernel_hint():
    """Warn early if no built guest is discoverable — every tool that boots a
    VM would otherwise fail deep inside a tool call with a confusing error."""
    env_kernel = os.environ.get("QOS_KERNEL")
    default = os.path.join(REPO, "build", "x86_64", "kernel.elf32")
    if env_kernel and os.path.exists(env_kernel):
        return None
    if os.path.exists(default):
        return None
    return (
        "No QuantumOS kernel found (looked at $QOS_KERNEL and "
        f"{default}). Build one first with `make kernel`, or point QOS_KERNEL "
        "at a kernel.elf32."
    )


def _server_params():
    """stdio parameters that launch scripts/qos_mcp.py as an MCP server.

    Running `python qos_mcp.py` puts scripts/ on sys.path[0], so its bare
    `from qos_bridge import ...` resolves; cwd is the repo root so any
    relative kernel path resolves the same way the CI gates see it. QOS_KERNEL
    (and the rest of the environment) is inherited by the child, so the VM the
    agent boots is the one you built."""
    from mcp import StdioServerParameters

    return StdioServerParameters(
        command=sys.executable,
        args=[os.path.join(HERE, "qos_mcp.py")],
        cwd=REPO,
        env=os.environ.copy(),
    )


async def list_tools_only():
    """Prove the Claude→MCP→qos_bridge plumbing end to end WITHOUT calling the
    API or needing a key: spawn the server, initialize, list the tools."""
    from mcp import ClientSession
    from mcp.client.stdio import stdio_client

    async with stdio_client(_server_params()) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await _open_tools(session)
            print(f"QuantumOS MCP server exposes {len(tools)} tools:")
            for t in sorted(tools, key=lambda x: x.name):
                summary = (t.description or "").strip().splitlines()[0] if t.description else ""
                print(f"  {t.name:<26} {summary[:90]}")
    return 0


async def run_agent(task, model, max_tokens, effort):
    """Drive QuantumOS with Claude: spawn the MCP server, convert its tools,
    and run the tool-runner loop, streaming the trace to stdout."""
    try:
        from anthropic import AsyncAnthropic
        from anthropic.lib.tools.mcp import async_mcp_tool
    except ImportError:
        print(
            "The `anthropic` SDK with MCP extras is required:\n"
            "  pip install -r requirements-claude-agent.txt",
            file=sys.stderr,
        )
        return 2

    from mcp import ClientSession
    from mcp.client.stdio import stdio_client

    try:
        client = AsyncAnthropic()  # resolves ANTHROPIC_API_KEY or an `ant auth login` profile
    except Exception as exc:  # noqa: BLE001 — surface any credential-resolution failure clearly
        print(
            f"Could not construct the Anthropic client ({exc}).\n"
            "Bring your own key: `export ANTHROPIC_API_KEY=sk-ant-...` or "
            "`ant auth login`.",
            file=sys.stderr,
        )
        return 2

    hint = _kernel_hint()
    if hint:
        print(f"WARNING: {hint}\n", file=sys.stderr)

    async with stdio_client(_server_params()) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            mcp_tools = await _open_tools(session)
            tools = [async_mcp_tool(t, session) for t in mcp_tools]
            print(
                f"Connected to QuantumOS ({len(tools)} tools). Model: {model}, "
                f"effort: {effort}.\n"
            )

            runner = client.beta.messages.tool_runner(
                model=model,
                max_tokens=max_tokens,
                # Adaptive thinking with a visible summary so the reasoning
                # between tool calls isn't a silent pause; effort is the depth
                # knob (high suits multi-step experiment design).
                thinking={"type": "adaptive", "display": "summarized"},
                output_config={"effort": effort},
                system=SYSTEM,
                messages=[{"role": "user", "content": task}],
                tools=tools,
            )

            # Each yielded message is one assistant turn (before its tools run).
            # The MCP tools execute long VM operations; the runner awaits them.
            async for message in runner:
                for block in message.content:
                    if block.type == "text" and block.text.strip():
                        print(block.text, flush=True)
                    elif block.type == "thinking" and getattr(block, "thinking", ""):
                        print(f"[thinking] {block.thinking}", flush=True)
                    elif block.type == "tool_use":
                        print(f"[tool] {block.name}({_fmt_input(block.input)})", flush=True)
                if message.stop_reason == "refusal":
                    print("\n[Claude declined this request.]", file=sys.stderr)
                    break
    return 0


def _fmt_input(obj):
    """Compact one-line rendering of a tool_use input for the trace."""
    if not obj:
        return ""
    return ", ".join(f"{k}={v!r}" for k, v in obj.items())


def main():
    p = argparse.ArgumentParser(
        description="Drive a live QuantumOS VM with Claude over its frozen MCP tools (BYO key)."
    )
    p.add_argument("task", nargs="?", default=DEFAULT_TASK,
                   help="the experiment for Claude to run (default: an associative-memory demo)")
    p.add_argument("--model", default=os.environ.get("QOS_CLAUDE_MODEL", "claude-opus-4-8"),
                   help="Anthropic model id (default claude-opus-4-8; try claude-fable-5 for the hardest reasoning)")
    p.add_argument("--max-tokens", type=int, default=16000)
    p.add_argument("--effort", default="high", choices=["low", "medium", "high", "xhigh", "max"])
    p.add_argument("--list-tools", action="store_true",
                   help="list the QuantumOS MCP tools and exit (no API call, no key needed)")
    args = p.parse_args()

    if args.list_tools:
        return asyncio.run(list_tools_only())
    return asyncio.run(run_agent(args.task, args.model, args.max_tokens, args.effort))


if __name__ == "__main__":
    sys.exit(main())
