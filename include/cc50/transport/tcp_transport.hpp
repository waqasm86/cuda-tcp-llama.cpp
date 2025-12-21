#pragma once
#include "transport.hpp"
#include <unordered_map>
#include <vector>

namespace cc50 {

class TcpTransport final : public ITransport {
public:
  TcpTransport();
  ~TcpTransport() override;

  Status start_server(const TransportOptions& opt, MessageHandler on_msg) override;
  Status start_client(const TransportOptions& opt, MessageHandler on_msg) override;
  Status send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) override;
  Status progress(int timeout_ms) override;

private:
  struct Conn {
    int fd{-1};
    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;
    size_t tx_off{0};
  };

  Status make_listen_socket(const std::string& host, uint16_t port);
  Status accept_new();
  Status handle_read(int fd);
  Status handle_write(int fd);

  Status queue_send(int fd, const uint8_t* bytes, size_t len);

  int ep_{-1};
  int listen_fd_{-1};
  int peer_fd_{-1}; // for client mode
  std::unordered_map<int, Conn> conns_;
  MessageHandler on_msg_;
  TransportOptions opt_;
  bool is_server_{false};
};

} // namespace cc50
