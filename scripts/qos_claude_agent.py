#!/usr/bin/env python3
"""Compat shim — the BYO-key agent moved to scripts/qos_agent.py, which now
speaks to ANY provider (Claude stays the default). This entrypoint preserves
the original Claude CLI exactly: `python3 scripts/qos_claude_agent.py ...`
behaves as it always did (provider=anthropic unless you say otherwise).

New capability lives in qos_agent.py, e.g. a local model with no key at all:

    python3 scripts/qos_agent.py --provider openai --model llama3.3 \
        --base-url http://localhost:11434/v1
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_agent import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
