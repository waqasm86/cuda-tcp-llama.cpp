#!/usr/bin/env bash
set -euo pipefail

LLAMA_SERVER_BIN="${LLAMA_SERVER_BIN:-./build-llama/bin/llama-server}"
MODEL="${MODEL:-/path/to/model.gguf}"

echo "[1] Starting llama-server..."
"${LLAMA_SERVER_BIN}" -m "${MODEL}" --host 127.0.0.1 --port 8080
