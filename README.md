# cuda-tcp-llama.cpp

[![C++](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![CUDA](https://img.shields.io/badge/CUDA-12.x-green.svg)](https://developer.nvidia.com/cuda-toolkit)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)
[![Build](https://img.shields.io/badge/build-CMake%20%2B%20Ninja-orange.svg)](https://cmake.org/)

> **High-performance C++/CUDA inference runtime bridging TCP client requests to llama.cpp server. Features dual backends, streaming responses, and comprehensive latency benchmarking.**

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
- [Prerequisites](#prerequisites)
- [Build](#build)
- [Usage](#usage)
  - [Start llama-server](#1-start-llama-server-terminal-1)
  - [Start TCP Bridge](#2-start-tcp-server-bridge-terminal-2)
  - [Run Client](#3-run-client-terminal-3)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [Performance](#performance)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

**cuda-tcp-llama.cpp** is a lightweight C++/CUDA mini inference runtime that bridges TCP requests to an external [llama.cpp `llama-server`](https://github.com/ggerganov/llama.cpp/tree/master/examples/server) instance running a GGUF model.

This project provides a **Python-free**, **systems-level** solution for deploying LLM inference with native CUDA integration and minimal dependencies.

### Why This Project?

- 🚫 **No Python dependencies** - Pure C++/CUDA implementation
- ⚡ **High performance** - Epoll-based I/O, zero-copy streaming
- 📊 **Built-in metrics** - Percentile latency measurement (mean/p50/p95/p99)
- 🔧 **Production ready** - Thread-safe, robust error handling
- 🎯 **Educational** - Clean architecture for studying inference servers

---

## Features

### ✨ Dual Backend Architecture
- **Toy Backend**: CUDA-accelerated synthetic workload generator for testing and benchmarking
- **Llama Server Backend**: HTTP client that forwards requests to llama.cpp's `llama-server` endpoint

### 📡 Advanced Network Transport
- Custom binary protocol with magic number validation
- Epoll-based non-blocking I/O for high concurrency
- Streaming response chunking with credit-based flow control
- Sub-millisecond TCP transport overhead

### 📊 Comprehensive Performance Metrics
- Real-time percentile latency tracking (mean, p50, p95, p99)
- Per-request timing with **microsecond precision**
- Configurable iteration counts for load testing
- Optional chunk-level output printing for debugging

### 🔧 Production-Ready Design
- Clean separation between transport, protocol, and backend layers
- Thread-safe work queue with condition variable synchronization
- Robust HTTP/1.1 client with chunked transfer encoding support
- JSON parsing with multiple schema fallbacks for llama.cpp compatibility

---

## Architecture

```
┌─────────────┐         TCP          ┌──────────────┐         HTTP         ┌─────────────┐
│             │  ───────────────►    │              │  ─────────────────►  │             │
│   Client    │  Binary Protocol     │  TCP Server  │  JSON POST/GET      │ llama.cpp   │
│  (cc50_llm  │  ◄───────────────    │  (cc50_llm   │  ◄─────────────────  │ llama-server│
│   _client)  │  Streaming Chunks    │   _server)   │  Streaming Response  │             │
└─────────────┘                      └──────────────┘                      └─────────────┘
                                            │
                                            ├─► Toy Backend (CUDA kernels)
                                            └─► Llama Server Backend (HTTP)
```

**Component Breakdown:**

1. **Client** (`cc50_llm_client`): Sends inference requests, measures latency, displays results
2. **TCP Server** (`cc50_llm_server`): Receives TCP requests, routes to backends, streams responses
3. **Backends**: 
   - **Toy**: Generates synthetic tokens using CUDA kernels
   - **Llama Server**: Forwards to external llama.cpp HTTP server
4. **Transport**: Epoll-based TCP with custom binary protocol
5. **Protocol**: Message framing, streaming chunks, error handling

---

## Prerequisites

- **OS**: Xubuntu 22.04 (or compatible Linux distribution)
- **CUDA Toolkit**: 12.x (tested with 12.8)
- **CMake**: 3.24 or higher
- **Ninja**: Build system
- **C++ Compiler**: GCC/Clang with C++20 support
- **llama.cpp**: For the `llama_server` backend (optional, not needed for toy backend)

### Installing Prerequisites

```bash
# Install build tools
sudo apt-get update
sudo apt-get install -y cmake ninja-build build-essential

# Install CUDA Toolkit (if not already installed)
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.0-1_all.deb
sudo dpkg -i cuda-keyring_1.0-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-toolkit-12-8
```

---

## Build

```bash
# Clone the repository
git clone https://github.com/waqasm86/cuda-tcp-llama.cpp.git
cd cuda-tcp-llama.cpp

# Configure with CMake
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=50

# Build with Ninja
ninja -C build
```

### Build Output
After successful build, you'll find:
- `build/cc50_llm_server` - TCP inference server
- `build/cc50_llm_client` - Benchmark client

---

## Usage

### Running with llama.cpp `llama-server` Backend

This is the typical production workflow using actual language models.

#### 1) Start llama-server (Terminal 1)

First, build llama.cpp with **`-DGGML_CUDA_FA=OFF`** configuration:

```bash
# In your llama.cpp repository
cd /path/to/llama.cpp
cmake -B build -DGGML_CUDA=ON -DGGML_CUDA_FA=OFF
cmake --build build --config Release

# Start the server
./build/bin/llama-server \
  -m /path/to/your/model.gguf \
  --host 127.0.0.1 \
  --port 8080 \
  --ctx-size 2048 \
  --n-gpu-layers 32
```

**Note**: This project does **not** manage model loading - `llama-server` owns that responsibility.

#### 2) Start TCP Server Bridge (Terminal 2)

```bash
./build/cc50_llm_server \
  --backend=llama_server \
  --listen=127.0.0.1:9199 \
  --llama-url=http://127.0.0.1:8080 \
  --llama-endpoint=/completion
```

**Available Options:**
- `--backend`: `toy` or `llama_server`
- `--listen`: Host:Port for TCP server
- `--llama-url`: Base URL of llama-server
- `--llama-endpoint`: Endpoint path (`/completion` or `/v1/completions`)
- `--max-tokens-default`: Default token limit (default: 128)

#### 3) Run Client (Terminal 3)

```bash
./build/cc50_llm_client \
  --server=127.0.0.1:9199 \
  --prompt "Explain TCP streaming in one paragraph." \
  --max-tokens 128 \
  --iters 5 \
  --print 1
```

**Client Options:**
- `--server`: TCP server address (Host:Port)
- `--prompt`: Input prompt string
- `--max-tokens`: Maximum tokens to generate
- `--iters`: Number of iterations for benchmarking
- `--print`: Print streamed chunks (0=off, 1=on)

**Expected Output:**
```
--- iter 0 ---
TCP streaming is a network communication method that...
[streamed text output]

iters=5 mean_ms=245.3 p50_ms=243.1 p95_ms=251.7 p99_ms=252.4
```

---

## Configuration

### Toy Backend Testing

For testing without llama.cpp:

```bash
# Terminal 1: Start server with toy backend
./build/cc50_llm_server \
  --backend=toy \
  --listen=127.0.0.1:9199

# Terminal 2: Run client
./build/cc50_llm_client \
  --server=127.0.0.1:9199 \
  --prompt "Test prompt" \
  --max-tokens 64 \
  --iters 10 \
  --print 1
```

The toy backend generates synthetic tokens using CUDA kernels for consistent benchmarking.

### Custom Endpoints

If your llama-server uses OpenAI-compatible endpoints:

```bash
./build/cc50_llm_server \
  --backend=llama_server \
  --listen=127.0.0.1:9199 \
  --llama-url=http://127.0.0.1:8080 \
  --llama-endpoint=/v1/completions
```

---

## Troubleshooting

### Transport Errors

**Problem**: Client reports `peer closed` or connection refused

**Solutions**:
- Verify both server and client use the same host:port
- Check firewalls: `sudo ufw status`
- Ensure server is running: `netstat -tlnp | grep 9199`
- Test connectivity: `telnet 127.0.0.1 9199`

### llama-server Response Schema Issues

**Problem**: Backend cannot parse llama-server JSON response

The backend tries these JSON keys in order:
1. `content`
2. `response`
3. `completion`
4. `text`

**Solution**: If your llama-server uses a different schema, update `src/backend/llama_server_backend.cpp`:

```cpp
// In LlamaServerBackend::infer_stream()
if (!json_extract_string(resp_body, "your_custom_key", text)) {
    return Status::Err("could not parse response");
}
```

### CUDA Errors

**Problem**: `cudaSetDevice failed` or CUDA runtime errors

**Solutions**:
- Verify CUDA installation: `nvidia-smi`
- Check CUDA version: `nvcc --version`
- Rebuild with correct architecture: `-DCMAKE_CUDA_ARCHITECTURES=<your-arch>`
- For GTX 750 Ti: use `50`, for RTX 30xx: use `86`

### Build Errors

**Problem**: CMake configuration fails

**Solutions**:
- Update CMake: `pip install --upgrade cmake`
- Install Ninja: `sudo apt-get install ninja-build`
- Check C++20 support: `g++ --version` (need GCC 10+)

---

## Performance

### Typical Latency Characteristics

| Component | Latency |
|-----------|---------|
| TCP transport overhead | < 100 μs |
| Streaming chunk delivery | Real-time (no buffering) |
| HTTP round-trip to llama-server | Depends on model/hardware |
| End-to-end client measurement | Percentile reporting available |

### Benchmark Example

```bash
# 100 iterations with a small model
./build/cc50_llm_client \
  --server=127.0.0.1:9199 \
  --prompt "Short test" \
  --max-tokens 32 \
  --iters 100 \
  --print 0

# Example output:
# iters=100 mean_ms=127.5 p50_ms=125.3 p95_ms=145.8 p99_ms=152.1
```

### Optimization Tips

1. **Use Unix sockets** for local connections (lower latency than TCP)
2. **Increase chunk size** in `LlamaServerOptions::chunk_bytes` for throughput
3. **Enable GPU layers** in llama-server (`--n-gpu-layers`)
4. **Tune epoll events** in `TransportOptions::epoll_max_events`

---

## Why C++/CUDA?

This project intentionally avoids Python to:
1. **Minimize dependencies** and runtime overhead
2. **Provide native CUDA integration** for GPU workloads
3. **Enable deployment** in systems-level and embedded contexts
4. **Offer educational value** for studying low-level ML infrastructure
5. **Match GGUF workflows** with llama.cpp's native C++ implementation

---

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

### How to Contribute

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Development Guidelines

- Follow C++20 best practices
- Add comments for complex logic
- Update documentation for new features
- Test with both backends (toy and llama_server)
- Run clang-format before committing

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

## Acknowledgments

- Built on top of [llama.cpp](https://github.com/ggerganov/llama.cpp) by Georgi Gerganov
- Inspired by systems-level ML inference research
- CUDA kernels based on standard practices from NVIDIA documentation

---

## Author

**Waqas Muhammad**
- GitHub: [@waqasm86](https://github.com/waqasm86)

---

## Related Projects

- [llama.cpp](https://github.com/ggerganov/llama.cpp) - Main llama.cpp repository
- [GGUF](https://github.com/ggerganov/ggml/blob/master/docs/gguf.md) - GGUF format specification

---

**Note**: This is a research/educational project. For production deployments, consider:
- Adding authentication/authorization
- Implementing request queuing and rate limiting
- Adding comprehensive logging and monitoring
- Hardening error handling for edge cases
