#pragma once
#include "backend.hpp"

#include <string>

namespace cc50 {

// Backend that calls an external llama.cpp `llama-server` over HTTP.
// This avoids ABI churn and keeps the transport/runtime layer independent from libllama.
//
// Expected server endpoints:
//   - POST /completion  (llama.cpp classic)  body: {"prompt": "...", "n_predict": 128, "stream": false}
//   - or POST /v1/completions (OpenAI-style) body: {"model":"", "prompt":"...", "max_tokens":128, "stream":false}
//
// We try /completion first by default and fall back to /v1/completions if needed.
struct LlamaServerOptions {
  std::string base_url {"http://127.0.0.1:8080"};
  std::string endpoint {"/completion"};        // default
  int connect_timeout_ms {2000};
  int request_timeout_ms {600000};             // 10 minutes
  size_t chunk_bytes {4096};                   // how we re-chunk output to our RESP_CHUNK messages
};

class LlamaServerBackend final : public IBackend {
public:
  explicit LlamaServerBackend(LlamaServerOptions opt = {}) : opt_(std::move(opt)) {}

  Status init() override;
  Status load_model(const std::string& path, int ctx, int threads) override; // no-op (server already has model)
  Status infer_stream(const InferRequest& req, StreamFn on_chunk, InferResult& out) override;

  void set_options(LlamaServerOptions o) { opt_ = std::move(o); }
  const LlamaServerOptions& options() const { return opt_; }

private:
  LlamaServerOptions opt_;

  // small helpers
  static std::string json_escape(std::string_view s);
  static bool json_extract_string(const std::string& body, const char* key, std::string& out);
};

} // namespace cc50
