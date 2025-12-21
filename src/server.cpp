#include "cc50/common.hpp"
#include "cc50/protocol.hpp"
#include "cc50/transport/tcp_transport.hpp"
#include "cc50/transport/ucx_transport.hpp"
#include "cc50/backend/toy_backend.hpp"
#include "cc50/backend/llama_server_backend.hpp"
#include "cc50/backend/backend.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <getopt.h>
#include <cstring>
#include <iostream>
#include <memory>

namespace cc50 {

struct ServerConfig {
  std::string transport{"tcp"};       // tcp|ucx
  std::string backend{"toy"};         // toy|llama_server
  std::string listen{"0.0.0.0:9199"};

  // kept for compatibility (toy ignores; llama_server ignores; llama-server itself loads the GGUF)
  std::string model{};

  int ctx{2048};
  int threads{4};
  uint32_t max_tokens_default{128};

  // llama-server HTTP options
  std::string llama_url{"http://127.0.0.1:8080"};
  std::string llama_endpoint{"/completion"};
};

static bool parse_hostport(const std::string& s, std::string& host, uint16_t& port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos) return false;
  host = s.substr(0, pos);
  port = std::stoi(s.substr(pos + 1));
  return true;
}

struct WorkItem {
  InferRequest req;
};

class ServerApp {
public:
  Status run(const ServerConfig& cfg) {
    cfg_ = cfg;

    // Select backend
    if (cfg_.backend == "llama_server") {
      LlamaServerOptions o;
      o.base_url = cfg_.llama_url;
      o.endpoint = cfg_.llama_endpoint;
      backend_ = std::make_unique<LlamaServerBackend>(o);
    } else {
      backend_ = std::make_unique<ToyBackend>();
    }

    auto st = backend_->init();
    if (!st.ok) return st;

    // (No-op for llama_server backend, kept for API consistency)
    st = backend_->load_model(cfg_.model, cfg_.ctx, cfg_.threads);
    if (!st.ok) return st;

    // Select transport
    if (cfg_.transport == "ucx") {
      transport_ = std::make_unique<UcxTransport>();
    } else {
      transport_ = std::make_unique<TcpTransport>();
    }

    TransportOptions opt;
    if (!parse_hostport(cfg_.listen, opt.listen_host, opt.listen_port)) {
      return Status::Err("bad --listen, expected HOST:PORT");
    }

    st = transport_->start_server(opt, [&](const IncomingMessage& msg) {
      on_msg(msg);
    });
    if (!st.ok) return st;

    // worker thread: consume queue and run inference
    worker_ = std::thread([&] { worker_loop(); });

    std::cout << "[server] transport=" << cfg_.transport
              << " backend=" << cfg_.backend
              << " listen=" << cfg_.listen << "\n";
    if (cfg_.backend == "llama_server") {
      std::cout << "[server] llama_url=" << cfg_.llama_url
                << " endpoint=" << cfg_.llama_endpoint << "\n";
    }

    while (!stop_) {
      auto ps = transport_->progress(50);
      if (!ps.ok) {
        std::cerr << "[server] transport error: " << ps.msg << "\n";
        break;
      }
    }

    // shutdown
    stop_ = true;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    return Status::Ok();
  }

private:
  void on_msg(const IncomingMessage& msg) {
    if (msg.type != (uint16_t)MsgType::REQ_INFER) return;
    if (msg.payload.size() < sizeof(InferRequestHdr)) return;

    InferRequestHdr rh{};
    std::memcpy(&rh, msg.payload.data(), sizeof(rh));
    if (msg.payload.size() < sizeof(rh) + rh.prompt_len) return;

    InferRequest req{};
    req.req_id = msg.req_id;
    req.max_tokens = rh.max_tokens ? rh.max_tokens : cfg_.max_tokens_default;
    req.credit_bytes = rh.credit_bytes;
    req.prompt.assign((const char*)msg.payload.data() + sizeof(rh), rh.prompt_len);

    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back(WorkItem{std::move(req)});
    }
    cv_.notify_one();
  }

  void worker_loop() {
    while (!stop_) {
      WorkItem wi;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (stop_) break;
        wi = std::move(q_.front());
        q_.pop_front();
      }

      InferResult res{};
      uint32_t sent_bytes = 0;
      uint32_t credit = wi.req.credit_bytes ? wi.req.credit_bytes : 256 * 1024;

      auto st = backend_->infer_stream(
        wi.req,
        [&](const std::string& chunk) {
          if (sent_bytes + chunk.size() > credit) return; // credit throttle
          transport_->send(wi.req.req_id, (uint16_t)MsgType::RESP_CHUNK,
                           (const uint8_t*)chunk.data(), chunk.size());
          sent_bytes += (uint32_t)chunk.size();
        },
        res
      );

      if (!st.ok) {
        const std::string& em = res.error.empty() ? st.msg : res.error;
        transport_->send(wi.req.req_id, (uint16_t)MsgType::RESP_ERR,
                         (const uint8_t*)em.data(), em.size());
      }

      InferDone done{};
      done.tokens = res.tokens;
      done.elapsed_us = res.elapsed_us;
      transport_->send(wi.req.req_id, (uint16_t)MsgType::RESP_DONE,
                       (const uint8_t*)&done, sizeof(done));
    }
  }

private:
  ServerConfig cfg_;
  std::unique_ptr<ITransport> transport_;
  std::unique_ptr<IBackend> backend_;

  std::atomic<bool> stop_{false};
  std::thread worker_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<WorkItem> q_;
};

} // namespace cc50

static void usage() {
  std::cerr << R"(cc50_llm_server
  --transport=tcp|ucx
  --backend=toy|llama_server
  --listen=HOST:PORT
  --max-tokens-default=128

  # llama_server backend options:
  --llama-url=http://127.0.0.1:8080
  --llama-endpoint=/completion   (or /v1/completions)

Example:
  # Terminal 1: start llama-server (from your llama.cpp build)
  #   ./build-llama/bin/llama-server -m /path/to/model.gguf --host 127.0.0.1 --port 8080
  #
  # Terminal 2: start UCX server that bridges UCX -> llama-server HTTP:
  #   ./build/bin/cc50_llm_server --transport=ucx --backend=llama_server --listen=127.0.0.1:9199 \
  #     --llama-url=http://127.0.0.1:8080 --llama-endpoint=/completion
)";
}

int main(int argc, char** argv) {
  cc50::ServerConfig cfg;

  static option opts[] = {
    {"transport", required_argument, nullptr, 't'},
    {"backend", required_argument, nullptr, 'b'},
    {"listen", required_argument, nullptr, 'l'},
    {"max-tokens-default", required_argument, nullptr, 'k'},
    {"llama-url", required_argument, nullptr, 'u'},
    {"llama-endpoint", required_argument, nullptr, 'e'},
    {"model", required_argument, nullptr, 'm'},
    {"ctx", required_argument, nullptr, 'c'},
    {"threads", required_argument, nullptr, 'p'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0}
  };

  while (true) {
    int idx = 0;
    int c = getopt_long(argc, argv, "t:b:l:k:u:e:m:c:p:h", opts, &idx);
    if (c == -1) break;
    switch (c) {
      case 't': cfg.transport = optarg; break;
      case 'b': cfg.backend = optarg; break;
      case 'l': cfg.listen = optarg; break;
      case 'k': cfg.max_tokens_default = (uint32_t)std::stoul(optarg); break;
      case 'u': cfg.llama_url = optarg; break;
      case 'e': cfg.llama_endpoint = optarg; break;
      case 'm': cfg.model = optarg; break;
      case 'c': cfg.ctx = std::stoi(optarg); break;
      case 'p': cfg.threads = std::stoi(optarg); break;
      case 'h': usage(); return 0;
      default: usage(); return 2;
    }
  }

  try {
    cc50::ServerApp app;
    auto st = app.run(cfg);
    if (!st.ok) {
      std::cerr << "error: " << st.msg << "\n";
      return 2;
    }
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 2;
  }

  return 0;
}
