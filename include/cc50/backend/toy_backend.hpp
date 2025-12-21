#pragma once
#include "backend.hpp"

namespace cc50 {

// CUDA toy backend: generates token-like chunks with GPU work so benchmarking is meaningful.
// Always builds, independent of llama.cpp.
class ToyBackend final : public IBackend {
public:
  Status init() override;
  Status load_model(const std::string& path, int ctx, int threads) override;
  Status infer_stream(const InferRequest& req, StreamFn on_chunk, InferResult& out) override;
};

} // namespace cc50
