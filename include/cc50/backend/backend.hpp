#pragma once

#include "../common.hpp"

#include <functional>
#include <string>

namespace cc50 {

struct InferRequest {
  uint64_t req_id{0};
  uint32_t max_tokens{64};
  uint32_t credit_bytes{256 * 1024};
  std::string prompt{};
};

using StreamFn = std::function<void(const std::string& chunk)>;

struct InferResult {
  uint32_t tokens{0};
  uint64_t elapsed_us{0};
  std::string text{};
  std::string error{};
};

class IBackend {
public:
  virtual ~IBackend() = default;
  virtual Status init() = 0;
  virtual Status load_model(const std::string& path, int ctx, int threads) = 0;
  virtual Status infer_stream(const InferRequest& req, StreamFn on_chunk, InferResult& out) = 0;
};

} // namespace cc50
