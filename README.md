# cuda-tcp-llama.cpp

cuda-tcp-llama.cpp is a lightweight C++/CUDA mini inference runtime that bridges TCP requests to an external [`llama.cpp` `llama-server`](https://github.com/ggerganov/llama.cpp/tree/master/examples/server) instance running a GGUF model.

## What you get

- `cc50_llm_server`
  - `--backend=toy|llama_server`
  - For the `llama_server` backend, it POSTs to `http://HOST:PORT/completion` (or `/v1/completions`) and streams the result back over a TCP socket.
- `cc50_llm_client`
  - sends prompt + max_tokens
  - measures **mean/p50/p95/p99** latency
  - optional `--print 1` to print streamed chunks

## Prereqs

- Xubuntu 22
- CUDA Toolkit 12.x (12.8 is already available in the container)

## Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=50

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

### 2) Start TCP server bridge (Terminal 2)

```bash
./build/bin/cc50_llm_server \
  --backend=llama_server \
  --listen=127.0.0.1:9199 \
  --llama-url=http://127.0.0.1:8080 \
  --llama-endpoint=/completion
```

### 3) Run client (Terminal 3)

```bash
./build/bin/cc50_llm_client \
  --server=127.0.0.1:9199 \
  --prompt "Explain TCP streaming in one paragraph." \
  --max-tokens 128 \
  --iters 5 \
  --print 1
```

You should see streamed text, then final stats like:

```
iters=5 mean_ms=... p50_ms=... p95_ms=... p99_ms=...
```

## Troubleshooting

### Transport errors
If the client reports `peer closed` or similar, ensure both the server and client are using the same host/port and that firewalls are not blocking local connections.

### llama-server response schema differs
This backend tries to parse common JSON keys:
- `content`
- `response`
- `completion`
- `text`

If your llama-server version returns a different schema, update `src/backend/llama_server_backend.cpp` (function `json_extract_string` usage).

## Why GGUF (llama.cpp) for this repo

This repo is intentionally C++/systems-focused. GGUF + llama.cpp keeps the serving story native and lightweight, matching CUDA workflows without pulling in a Python runtime.
