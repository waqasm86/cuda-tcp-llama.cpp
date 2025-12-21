#pragma once
#include "transport.hpp"

#ifdef CC50_ENABLE_UCX
#include <ucp/api/ucp.h>
#endif

#include <vector>
#include <unordered_map>

namespace cc50 {

class UcxTransport final : public ITransport {
public:
  UcxTransport();
  ~UcxTransport() override;

  Status start_server(const TransportOptions& opt, MessageHandler on_msg) override;
  Status start_client(const TransportOptions& opt, MessageHandler on_msg) override;
  Status send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) override;
  Status progress(int timeout_ms) override;

private:
#ifdef CC50_ENABLE_UCX
  static void on_conn_request(ucp_conn_request_h req, void* arg);
  static void on_client_ep_close(void* arg);

  Status init_ucx_common();
  Status create_worker_with_wakeup();
  Status create_listener();
  Status connect_to_server();

  Status send_bytes(const uint8_t* bytes, size_t len, uint64_t tag);
  Status post_recv(uint64_t tag);

  void drain_completions();
  void pump_recv();

  ucp_context_h ucp_ctx_{nullptr};
  ucp_worker_h  worker_{nullptr};
  ucp_listener_h listener_{nullptr};
  ucp_ep_h ep_{nullptr};

  int epoll_fd_{-1};
  int ucx_efd_{-1};

  struct RecvBuf {
    std::vector<uint8_t> buf;
    ucp_tag_message_h msg{nullptr};
  };
  std::vector<uint8_t> rx_buf_;
  bool is_server_{false};

  MessageHandler on_msg_;
  TransportOptions opt_;
#endif
};

} // namespace cc50
