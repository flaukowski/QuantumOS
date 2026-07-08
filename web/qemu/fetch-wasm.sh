#!/usr/bin/env bash
# Fetch the large qemu-system-x86_64.wasm (~40 MB) that this demo runs on.
#
# It is NOT committed to the repo (it would bloat every clone). Both the CI
# boot gate and the GitHub Pages deploy run this to drop it into web/qemu/
# before serving. Pinned to a specific build and integrity-checked; override
# QOS_QEMU_WASM_URL to self-host it (e.g. from a QuantumOS release asset).
#
# Provenance: an unmodified qemu-system-x86_64 compiled to WebAssembly by
# ktock/qemu-wasm (GPLv2), taken from the project's own hosted x86_64 demo.
set -euo pipefail

DEST="$(dirname "$0")/qemu-system-x86_64.wasm"
URL="${QOS_QEMU_WASM_URL:-https://ktock.github.io/qemu-wasm-demo/images/alpine-x86_64/qemu-system-x86_64.wasm}"
SHA256="f53107a35029d676aa551cd00d042f4f65af39a89bf72464494321fafdf54191"

if [ -f "$DEST" ] && echo "$SHA256  $DEST" | sha256sum -c - >/dev/null 2>&1; then
  echo "qemu-system-x86_64.wasm already present and verified."
  exit 0
fi

echo "Fetching qemu-system-x86_64.wasm from $URL ..."
curl -fSL "$URL" -o "$DEST"
echo "$SHA256  $DEST" | sha256sum -c -
echo "OK."
