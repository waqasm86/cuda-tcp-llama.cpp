#include "cc50/transport/ucx_transport.hpp"
#include "cc50/protocol.hpp"

#ifdef CC50_ENABLE_UCX

#include <arpa/inet.h>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>

namespace cc50 {

static Status ucx_wait(ucp_worker_h worker, void *req) {
  if (req == nullptr) return Status::Ok();
  if (UCS_PTR_IS_ERR(req)) return Status::Err("UCX request error");

  while (ucp_request_check_status(req) == UCS_INPROGRESS) {
    ucp_worker_progress(worker);
  }

  ucs_status_t st = ucp_request_check_status(req);
  ucp_request_free(req);

  if (st != UCS_OK) {
    return Status::Err(std::string("UCX request failed: ") + ucs_status_string(st));
  }
  return Status::Ok();
}


static Status ucx_err(const char* where, ucs_status_t st) {
  return Status::Err(std::string(where) + ": " + ucs_status_string(st));
}

UcxTransport::UcxTransport() {}
UcxTransport::~UcxTransport() {
  if (listener_) ucp_listener_destroy(listener_);
  if (ep_)       ucp_ep_destroy(ep_);
  if (worker_)   ucp_worker_destroy(worker_);
  if (ucp_ctx_)  ucp_cleanup(ucp_ctx_);
  if (epoll_fd_ >= 0) close(epoll_fd_);
}

Status UcxTransport::init_ucx_common() {
  ucp_params_t params;
  std::memset(&params, 0, sizeof(params));
  params.field_mask = UCP_PARAM_FIELD_FEATURES;
  params.features   = UCP_FEATURE_TAG;

  ucp_config_t* config = nullptr;
  ucs_status_t st = ucp_config_read(nullptr, nullptr, &config);
  if (st != UCS_OK) return ucx_err("ucp_config_read", st);

  st = ucp_init(&params, config, &ucp_ctx_);
  ucp_config_release(config);
  if (st != UCS_OK) return ucx_err("ucp_init", st);

  return Status::Ok();
}

Status UcxTransport::create_worker_with_wakeup() {
  ucp_worker_params_t wparams;
  std::memset(&wparams, 0, sizeof(wparams));
  wparams.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE |
                       UCP_WORKER_PARAM_FIELD_EVENTS;
  wparams.thread_mode = UCS_THREAD_MODE_SINGLE;
  wparams.events = UCP_WAKEUP_RX | UCP_WAKEUP_TX;

  ucs_status_t st = ucp_worker_create(ucp_ctx_, &wparams, &worker_);
  if (st != UCS_OK) return ucx_err("ucp_worker_create", st);

  // Create epoll and register UCX wakeup FD
  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) return Status::Err("epoll_create1 failed");

  int efd = -1;
  st = ucp_worker_get_efd(worker_, &efd);
  if (st != UCS_OK) return ucx_err("ucp_worker_get_efd", st);
  ucx_efd_ = efd;

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = ucx_efd_;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ucx_efd_, &ev) < 0) {
    return Status::Err("epoll_ctl add ucx efd failed");
  }
  return Status::Ok();
}

void UcxTransport::on_conn_request(ucp_conn_request_h req, void* arg) {
  auto* self = reinterpret_cast<UcxTransport*>(arg);
  if (!self || self->ep_) return;

  ucp_ep_params_t epp;
  std::memset(&epp, 0, sizeof(epp));
  epp.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST |
                   UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
  epp.conn_request = req;
  epp.err_mode = UCP_ERR_HANDLING_MODE_PEER;

  ucs_status_t st = ucp_ep_create(self->worker_, &epp, &self->ep_);
  if (st != UCS_OK) {
    self->ep_ = nullptr;
  }
}

Status UcxTransport::create_listener() {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt_.listen_port);
  if (::inet_pton(AF_INET, opt_.listen_host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for listen host");
  }

  ucp_listener_params_t lp;
  std::memset(&lp, 0, sizeof(lp));
  lp.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                  UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lp.sockaddr.addr = (const struct sockaddr*)&addr;
  lp.sockaddr.addrlen = sizeof(addr);
  lp.conn_handler.cb = &UcxTransport::on_conn_request;
  lp.conn_handler.arg = this;

  ucs_status_t st = ucp_listener_create(worker_, &lp, &listener_);
  if (st != UCS_OK) return ucx_err("ucp_listener_create", st);

  return Status::Ok();
}

Status UcxTransport::connect_to_server() {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt_.server_port);
  if (::inet_pton(AF_INET, opt_.server_host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for server host");
  }

  ucp_ep_params_t epp;
  std::memset(&epp, 0, sizeof(epp));
  epp.field_mask = UCP_EP_PARAM_FIELD_FLAGS |
                   UCP_EP_PARAM_FIELD_SOCK_ADDR |
                   UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
  epp.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  epp.sockaddr.addr = (const struct sockaddr*)&addr;
  epp.sockaddr.addrlen = sizeof(addr);
  epp.err_mode = UCP_ERR_HANDLING_MODE_PEER;

  ucs_status_t st = ucp_ep_create(worker_, &epp, &ep_);
  if (st != UCS_OK) return ucx_err("ucp_ep_create(client)", st);
  return Status::Ok();
}

Status UcxTransport::start_server(const TransportOptions& opt, MessageHandler on_msg) {
  is_server_ = true;
  opt_ = opt;
  on_msg_ = std::move(on_msg);

  auto s = init_ucx_common();
  if (!s.ok) return s;
  s = create_worker_with_wakeup();
  if (!s.ok) return s;
  s = create_listener();
  if (!s.ok) return s;

  // Pre-post a receive for request tag
  rx_buf_.resize(1024 * 1024);
  return Status::Ok();
}

Status UcxTransport::start_client(const TransportOptions& opt, MessageHandler on_msg) {
  is_server_ = false;
  opt_ = opt;
  on_msg_ = std::move(on_msg);

  auto s = init_ucx_common();
  if (!s.ok) return s;
  s = create_worker_with_wakeup();
  if (!s.ok) return s;
  s = connect_to_server();
  if (!s.ok) return s;

  rx_buf_.resize(1024 * 1024);
  return Status::Ok();
}

// Simple tag scheme:
// - requests use tag 0xCC50_0001
// - responses use tag 0xCC50_0002
static constexpr uint64_t TAG_REQ  = 0xCC500001ull;
static constexpr uint64_t TAG_RESP = 0xCC500002ull;

Status UcxTransport::send_bytes(const uint8_t* bytes, size_t len, uint64_t tag) {
  if (!ep_) return Status::Err("UCX endpoint not connected yet");
  void* req = ucp_tag_send_nb(ep_, bytes, len, ucp_dt_make_contig(1), tag, nullptr);
  return ucx_wait(worker_, req);
}

Status UcxTransport::send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) {
  // Pack as: MsgHeader + payload
  MsgHeader h{};
  h.magic = kMagic;
  h.version = kProtoVer;
  h.type = type;
  h.req_id = req_id;
  h.flags = 0;
  h.length = static_cast<uint32_t>(len);

  std::vector<uint8_t> buf;
  buf.resize(sizeof(MsgHeader) + len);
  std::memcpy(buf.data(), &h, sizeof(MsgHeader));
  if (len) std::memcpy(buf.data() + sizeof(MsgHeader), data, len);

  uint64_t tag = (type == (uint16_t)MsgType::REQ_INFER) ? TAG_REQ : TAG_RESP;
  return send_bytes(buf.data(), buf.size(), tag);
}

void UcxTransport::pump_recv() {
  if (!worker_) return;

  // Probe for either request or response depending on mode.
  uint64_t tag = is_server_ ? TAG_REQ : TAG_RESP;

  while (true) {
    ucp_tag_recv_info_t info{};
    ucp_tag_message_h msg =
        ucp_tag_probe_nb(worker_, tag, 0xffffffffffffffffull, 1, &info);
    if (!msg) break;

    size_t len = info.length;
    if (len > rx_buf_.size()) {
      // either resize or skip (choose what fits your protocol)
      rx_buf_.resize(len);
    }

    void* req = ucp_tag_msg_recv_nb(worker_, rx_buf_.data(), len, ucp_dt_make_contig(1), msg, nullptr);
    if (UCS_PTR_IS_ERR(req)) break;

		auto st = ucx_wait(worker_, req);
		if (!st.ok) break;
    

    if (len < sizeof(MsgHeader)) continue;
    MsgHeader h{};
    std::memcpy(&h, rx_buf_.data(), sizeof(MsgHeader));
    if (h.magic != kMagic) continue;

    IncomingMessage im{};
    im.req_id = h.req_id;
    im.type = h.type;
    size_t plen = len - sizeof(MsgHeader);
    im.payload.resize(plen);
    if (plen) std::memcpy(im.payload.data(), rx_buf_.data() + sizeof(MsgHeader), plen);

    if (on_msg_) on_msg_(im);
  }
}

Status UcxTransport::progress(int timeout_ms) {
  // Arm wakeup then epoll wait
  if (!worker_) return Status::Err("UCX worker not initialized");

  // Let UCX enable wakeups
  (void) ucp_worker_arm(worker_);

  epoll_event ev{};
  int n = epoll_wait(epoll_fd_, &ev, 1, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return Status::Ok();
    return Status::Err("epoll_wait failed");
  }

  // Progress UCX until idle
  while (ucp_worker_progress(worker_) != 0) {}
  pump_recv();
  return Status::Ok();
}

} // namespace cc50

#else
// CC50_ENABLE_UCX not defined
namespace cc50 {
UcxTransport::UcxTransport() {}
UcxTransport::~UcxTransport() {}
Status UcxTransport::start_server(const TransportOptions&, MessageHandler) { return Status::Err("UCX not enabled"); }
Status UcxTransport::start_client(const TransportOptions&, MessageHandler) { return Status::Err("UCX not enabled"); }
Status UcxTransport::send(uint64_t, uint16_t, const uint8_t*, size_t) { return Status::Err("UCX not enabled"); }
Status UcxTransport::progress(int) { return Status::Err("UCX not enabled"); }
} // namespace cc50
#endif
