#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "../common.hpp"

namespace cc50 {

struct IncomingMessage {
  uint64_t req_id{};
  uint16_t type{};
  std::vector<uint8_t> payload;
};

using MessageHandler = std::function<void(const IncomingMessage&)>;

struct TransportOptions {
  std::string listen_host{"0.0.0.0"};
  uint16_t listen_port{9199};
  std::string server_host{"127.0.0.1"};
  uint16_t server_port{9199};
  int epoll_max_events{256};
};

class ITransport {
public:
  virtual ~ITransport() = default;
  virtual Status start_server(const TransportOptions& opt, MessageHandler on_msg) = 0;
  virtual Status start_client(const TransportOptions& opt, MessageHandler on_msg) = 0;

  // Send a message (header + payload). For clients, sends to server. For server, sends to a peer endpoint.
  virtual Status send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) = 0;

  // Drive progress (poll/epoll). Returns when one tick is done.
  virtual Status progress(int timeout_ms) = 0;
};

} // namespace cc50
