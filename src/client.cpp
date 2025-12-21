#include "cc50/common.hpp"
#include "cc50/protocol.hpp"
#include "cc50/transport/tcp_transport.hpp"
#include "cc50/transport/ucx_transport.hpp"

#include <getopt.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <cstring>
#include <memory>

namespace cc50 {

struct ClientConfig {
  std::string transport{"tcp"};
  std::string server{"127.0.0.1:9199"};
  std::string prompt{"Hello from UCX client. Write one sentence."};
  uint32_t max_tokens{64};
  uint32_t iters{10};
  bool print_chunks{false};
};

static bool parse_hostport(const std::string& s, std::string& host, int& port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos) return false;
  host = s.substr(0, pos);
  port = std::stoi(s.substr(pos + 1));
  return true;
}

static double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  double idx = (p / 100.0) * (v.size() - 1);
  size_t i = (size_t)idx;
  double frac = idx - i;
  if (i + 1 < v.size()) return v[i] * (1 - frac) + v[i + 1] * frac;
  return v.back();
}

} // namespace cc50

static void usage() {
  std::cerr << R"(cc50_llm_client
  --transport=tcp|ucx
  --server=HOST:PORT
  --prompt="..."
  --max-tokens=64
  --iters=10
  --print=0|1

Example:
  ./build/bin/cc50_llm_client --transport=ucx --server=127.0.0.1:9199 \
    --prompt "Explain UCX in one paragraph" --max-tokens 128 --iters 5 --print 1
)";
}

int main(int argc, char** argv) {
  cc50::ClientConfig cfg;

  static option opts[] = {
    {"transport", required_argument, nullptr, 't'},
    {"server", required_argument, nullptr, 's'},
    {"prompt", required_argument, nullptr, 'p'},
    {"max-tokens", required_argument, nullptr, 'k'},
    {"iters", required_argument, nullptr, 'i'},
    {"print", required_argument, nullptr, 'P'},
    {"help", no_argument, nullptr, 'h'},
    {nullptr, 0, nullptr, 0}
  };

  while (true) {
    int idx = 0;
    int c = getopt_long(argc, argv, "t:s:p:k:i:P:h", opts, &idx);
    if (c == -1) break;
    switch (c) {
      case 't': cfg.transport = optarg; break;
      case 's': cfg.server = optarg; break;
      case 'p': cfg.prompt = optarg; break;
      case 'k': cfg.max_tokens = (uint32_t)std::stoul(optarg); break;
      case 'i': cfg.iters = (uint32_t)std::stoul(optarg); break;
      case 'P': cfg.print_chunks = (std::stoi(optarg) != 0); break;
      case 'h': usage(); return 0;
      default: usage(); return 2;
    }
  }

  std::unique_ptr<cc50::ITransport> tr;
  if (cfg.transport == "ucx") tr = std::make_unique<cc50::UcxTransport>();
  else tr = std::make_unique<cc50::TcpTransport>();

  cc50::TransportOptions opt;
  {
    std::string host; int port=0;
    if (!cc50::parse_hostport(cfg.server, host, port)) {
      std::cerr << "bad --server, expected HOST:PORT\n";
      return 2;
    }
    opt.server_host = host;
    opt.server_port = port;
  }

  std::atomic<bool> got_done{false};
  std::atomic<bool> got_err{false};
  uint64_t cur_req{0};
  uint64_t t0{0};

  std::vector<double> lats_ms;
  lats_ms.reserve(cfg.iters);

  auto st = tr->start_client(opt, [&](const cc50::IncomingMessage& msg) {
    if (msg.req_id != cur_req) return;

    if (msg.type == (uint16_t)cc50::MsgType::RESP_CHUNK) {
      if (cfg.print_chunks && !msg.payload.empty()) {
        std::cout.write((const char*)msg.payload.data(), (std::streamsize)msg.payload.size());
        std::cout.flush();
      }
    } else if (msg.type == (uint16_t)cc50::MsgType::RESP_DONE) {
      uint64_t t1 = cc50::now_us();
      lats_ms.push_back((t1 - t0) / 1000.0);
      got_done = true;
    } else if (msg.type == (uint16_t)cc50::MsgType::RESP_ERR) {
      got_err = true;
      got_done = true;
      if (!msg.payload.empty()) {
        std::string em((const char*)msg.payload.data(), msg.payload.size());
        std::cerr << "\n[client] server error: " << em << "\n";
      }
    }
  });

  if (!st.ok) {
    std::cerr << "start_client failed: " << st.msg << "\n";
    return 2;
  }

  cc50::InferRequestHdr rh{};
  rh.max_tokens = cfg.max_tokens;
  rh.credit_bytes = 256 * 1024;
  rh.prompt_len = (uint32_t)cfg.prompt.size();

  std::vector<uint8_t> payload(sizeof(rh) + cfg.prompt.size());
  std::memcpy(payload.data(), &rh, sizeof(rh));
  std::memcpy(payload.data() + sizeof(rh), cfg.prompt.data(), cfg.prompt.size());

  for (uint32_t i = 0; i < cfg.iters; i++) {
    got_done = false;
    got_err = false;
    cur_req = (uint64_t)cc50::now_us() ^ ((uint64_t)i << 32);
    t0 = cc50::now_us();

    if (cfg.print_chunks) std::cout << "\n--- iter " << i << " ---\n";
    tr->send(cur_req, (uint16_t)cc50::MsgType::REQ_INFER, payload.data(), payload.size());

    while (!got_done) tr->progress(50);
    if (cfg.print_chunks) std::cout << "\n";
  }

  double p50 = cc50::percentile(lats_ms, 50);
  double p95 = cc50::percentile(lats_ms, 95);
  double p99 = cc50::percentile(lats_ms, 99);

  double sum = 0;
  for (auto x : lats_ms) sum += x;
  double mean = lats_ms.empty() ? 0.0 : (sum / lats_ms.size());

  std::cout << "iters=" << lats_ms.size()
            << " mean_ms=" << mean
            << " p50_ms=" << p50
            << " p95_ms=" << p95
            << " p99_ms=" << p99 << "\n";

  return got_err ? 2 : 0;
}
