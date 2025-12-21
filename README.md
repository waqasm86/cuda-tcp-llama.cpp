# cc50-ucx-llama-server-bridge

A **C++/CUDA + UCX** mini inference runtime that **bridges UCX requests to an external `llama.cpp` `llama-server`** running a **GGUF** model.

Why this design:
- Your UCX/CUDA runtime stays stable and independent of libllama ABI churn.
- You can hard-disable/avoid llama.cpp internal flash-attention issues in your build of llama.cpp.
- This still proves you can build an LLM inference stack: transport (UCX), protocol, scheduling, streaming, and GPU work.

## What you get

- `cc50_llm_server`
  - `--transport=tcp|ucx`
  - `--backend=toy|llama_server`
  - For `llama_server` backend, it POSTs to `http://HOST:PORT/completion` (or `/v1/completions`) and streams the result back over UCX/TCP.
- `cc50_llm_client`
  - sends prompt + max_tokens
  - measures **mean/p50/p95/p99** latency
  - optional `--print 1` to print streamed chunks

## Prereqs

- Xubuntu 22
- CUDA Toolkit 12.x (you already have 12.8)
- UCX installed with `ucx.pc` visible to pkg-config (you installed UCX under `/usr/local`)

If UCX was installed under `/usr/local`, export:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

## Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=50 \
  -DCC50_ENABLE_UCX=ON

ninja -C build
```

## Run with llama.cpp `llama-server` (GGUF)

### 1) Start llama-server (Terminal 1)

Use your rebuilt llama.cpp that was configured with **`-DGGML_CUDA_FA=OFF`**.

Example (adjust paths to your build + model):

```bash
# from llama.cpp repo
./build-llama/bin/llama-server \
  -m /path/to/model.gguf \
  --host 127.0.0.1 \
  --port 8080
```

> If your llama-server has extra flags (ctx, gpu layers, etc.) set them here.  
> This project does **not** try to manage model loading; llama-server owns that.

### 2) Start UCX server bridge (Terminal 2)

```bash
./build/bin/cc50_llm_server \
  --transport=ucx \
  --backend=llama_server \
  --listen=127.0.0.1:9199 \
  --llama-url=http://127.0.0.1:8080 \
  --llama-endpoint=/completion
```

### 3) Run client (Terminal 3)

```bash
./build/bin/cc50_llm_client \
  --transport=ucx \
  --server=127.0.0.1:9199 \
  --prompt "Explain UCX vs TCP in one paragraph." \
  --max-tokens 128 \
  --iters 5 \
  --print 1
```

You should see streamed text, then final stats like:

```
iters=5 mean_ms=... p50_ms=... p95_ms=... p99_ms=...
```

## Troubleshooting

### UCX not found at build time
If CMake warns that `ucx` pkg-config module isn't found:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
rm -rf build
```

Then re-run CMake.

### llama-server response schema differs
This backend tries to parse common JSON keys:
- `content`
- `response`
- `completion`
- `text`

If your llama-server version returns a different schema, update:
`src/backend/llama_server_backend.cpp` (function `json_extract_string` usage).

## Why GGUF (llama.cpp) for this repo
This repo is intentionally C++/systems-focused. GGUF + llama.cpp keeps the serving story native and lightweight, matching UCX/CUDA workflows more directly than a PyTorch/HuggingFace stack.
# ucx-cuda
