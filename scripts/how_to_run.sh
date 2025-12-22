#!/usr/bin/env bash
set -euo pipefail

echo "[2] Start bridge server:"
echo "  ./build/bin/cc50_llm_server --backend=llama_server --listen=127.0.0.1:9199 --llama-url=http://127.0.0.1:8080 --llama-endpoint=/completion"
echo
echo "[3] Run client:"
echo "  ./build/bin/cc50_llm_client --server=127.0.0.1:9199 --prompt \"Hello\" --max-tokens 64 --iters 3 --print 1"
