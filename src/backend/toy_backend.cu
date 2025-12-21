#include "cc50/backend/toy_backend.hpp"
#include <cuda_runtime.h>
#include <cstring>

namespace cc50 {

static __global__ void spin_kernel(uint32_t iters, uint32_t* out) {
  uint32_t x = threadIdx.x + blockIdx.x * blockDim.x;
  for (uint32_t i = 0; i < iters; i++) {
    x = x * 1664525u + 1013904223u;
  }
  if (threadIdx.x == 0 && blockIdx.x == 0) *out = x;
}

Status ToyBackend::init() {
  int dev = 0;
  cudaError_t e = cudaSetDevice(dev);
  if (e != cudaSuccess) return Status::Err(std::string("cudaSetDevice failed: ") + cudaGetErrorString(e));
  return Status::Ok();
}

Status ToyBackend::load_model(const std::string&, int, int) {
  return Status::Ok();
}

Status ToyBackend::infer_stream(const InferRequest& req, StreamFn on_chunk, InferResult& out) {
  const uint64_t t0 = now_us();
  out.tokens = 0;
  out.elapsed_us = 0;
  out.text.clear();
  out.error.clear();

  // Allocate a tiny device buffer
  uint32_t* d_out = nullptr;
  cudaMalloc(&d_out, sizeof(uint32_t));

  // Do "work" roughly proportional to max_tokens so it benchmarks nicely
  const uint32_t iters = 20000u;
  for (uint32_t i = 0; i < req.max_tokens; i++) {
    spin_kernel<<<8, 256>>>(iters, d_out);
    cudaDeviceSynchronize();

    // Emit a small chunk
    if (on_chunk) on_chunk(" token");
    out.text += " token";
    out.tokens++;
  }

  cudaFree(d_out);

  out.elapsed_us = now_us() - t0;
  return Status::Ok();
}

} // namespace cc50
