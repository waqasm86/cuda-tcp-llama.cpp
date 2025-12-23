# CUDA TCP LLaMA Inference Server

**Low-Latency GPU-Aware Inference Data Plane**

[![C++](https://img.shields.io/badge/C%2B%2B-17%2F20-blue.svg)](https://isocpp.org/)
[![CUDA](https://img.shields.io/badge/CUDA-12.x-green.svg)](https://developer.nvidia.com/cuda-toolkit)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)

---

## Overview

This project implements a high-performance TCP-based inference data plane in modern C++, designed to serve CUDA-accelerated LLM inference using [llama.cpp](https://github.com/ggerganov/llama.cpp) as the backend.

The goal is to explore low-level networking, concurrency, and GPU integration challenges that arise when building inference infrastructure under tight hardware constraints, such as limited VRAM and legacy GPUs.

Rather than relying on high-level frameworks, this project focuses on **explicit control** over networking, memory usage, and request scheduling, mirroring real-world inference serving environments.

---

## Architecture

### Control Plane
- Process lifecycle management
- Server configuration
- Backend selection (llama.cpp CUDA backend)

### Data Plane
- Custom TCP binary protocol
- `epoll`-based non-blocking I/O
- Concurrent request handling
- Explicit backpressure control

### Inference Runtime
- CUDA-backed llama.cpp
- GGUF quantized models
- Optimized for low-VRAM environments (≤1 GB)

---

## Key Features

### Custom TCP Protocol
- Minimal binary framing for inference requests/responses
- Designed for predictable latency and low overhead

### epoll-Driven Concurrency
- Non-blocking socket I/O
- Scales concurrent clients without thread-per-connection overhead

### GPU-Aware Inference
- Direct integration with CUDA-enabled llama.cpp
- Optimized for constrained GPUs (Compute Capability 5.0)

### Performance Instrumentation
- Latency tracking (p50 / p95 / p99)
- Throughput measurement under concurrent load

---

## Why TCP (Before RDMA)

TCP was intentionally chosen as the baseline transport:

- **Universally available**
- **Easy to debug and profile**
- **Provides a reference point** for evaluating RDMA / UCX trade-offs

The design cleanly separates transport logic from inference execution, enabling future extension to RDMA or GPU-direct transports without rewriting the inference core.

---

## Build & Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/bin/llama_tcp_server \
  --model /path/to/model.gguf \
  --listen 0.0.0.0:8080
```

---

## Design Goals

- **Explicit control** over data movement
- **Predictable latency** under load
- **Minimal abstraction overhead**
- **Clear separation** between networking and inference logic

---

## Relation to Distributed Systems

Although runnable on a single node, this project implements distributed-systems primitives:

- Network-based request routing
- Stateless serving
- Backpressure and flow control
- Separation of control plane and data plane

It is designed to serve as a **foundation layer** for multi-node inference systems using MPI, RDMA, or GPU-aware transports.

---

## Technology Stack

- **C++17 / C++20**
- **CUDA**
- **epoll / POSIX sockets**
- **llama.cpp** (GGUF models)
- **CMake**

---

## Author

**Mohammad Waqas**
GitHub: [https://github.com/waqasm86](https://github.com/waqasm86)

---

## Project Repository

[https://github.com/waqasm86/cuda-tcp-llama.cpp](https://github.com/waqasm86/cuda-tcp-llama.cpp)
