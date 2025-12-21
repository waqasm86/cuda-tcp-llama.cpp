#include "cc50/transport/ucx_transport.hpp"
#include "cc50/protocol.hpp"

#if CC50_ENABLE_UCX

#include <arpa/inet.h>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace cc50 {

// ---------------------------- helpers ----------------------------

static Status ucx_err(const char* where, ucs_status_t st) {
  return Status::Err(std::string(where) + ": " + ucs_status_string(st));
}

static Status ucx_wait(ucp_worker_h worker, void* req) {
  if (req == nullptr) return Status::Ok();
  if (UCS_PTR_IS_ERR(req)) {
    ucs_status_t st = UCS_PTR_STATUS(req);
    return Status::Err(std::string("UCX request error: ") + ucs_status_string(st));
  }

  ucs_status_t st = UCS_INPROGRESS;
  while ((st = ucp_request_check_status(req)) == UCS_INPROGRESS) {
    ucp_worker_progress(worker);
  }

  ucp_request_free(req);

  if (st != UCS_OK) {
    return Status::Err(std::string("UCX request failed: ") + ucs_status_string(st));
  }
  return Status::Ok();
}

static constexpr uint64_t TAG_REQ  = 0xCC500001ull;
static constexpr uint64_t TAG_RESP = 0xCC500002ull;

// Collect received messages while holding mu_.
// IMPORTANT: this does NOT call on_msg_ while the mutex is held.
static void pump_recv_locked(UcxTransport* self, std::vector<IncomingMessage>& out) {
  if (!self || !self->worker_) return;

  const uint64_t tag      = self->is_server_ ? TAG_REQ : TAG_RESP;
  const uint64_t tag_mask = 0xFFFFFFFFFFFFFFFFull;

  for (;;) {
    ucp_tag_recv_info_t info;
    std::memset(&info, 0, sizeof(info));

    ucp_tag_message_h msg = ucp_tag_probe_nb(self->worker_, tag, tag_mask, 1, &info);
    if (!msg) break;

    const size_t len = info.length;
    if (len < sizeof(MsgHeader)) {
      // Consume it anyway
      std::vector<uint8_t> tmp(len);
      ucp_request_param_t p;
      std::memset(&p, 0, sizeof(p));
      void* req = ucp_tag_msg_recv_nbx(self->worker_, tmp.data(), len, msg, &p);
      (void)ucx_wait(self->worker_, req);
      continue;
    }

    if (len > self->rx_buf_.size()) {
      self->rx_buf_.resize(len);
    }

    ucp_request_param_t param;
    std::memset(&param, 0, sizeof(param));
    param.op_attr_mask = 0;

    void* req = ucp_tag_msg_recv_nbx(self->worker_, self->rx_buf_.data(), len, msg, &param);
    if (UCS_PTR_IS_ERR(req)) {
      std::cerr << "[UCX] Receive error: " << ucs_status_string(UCS_PTR_STATUS(req)) << "\n";
      break;
    }

    Status st = ucx_wait(self->worker_, req);
    if (!st.ok) {
      std::cerr << "[UCX] Wait failed: " << st.msg << "\n";
      break;
    }

    MsgHeader h;
    std::memcpy(&h, self->rx_buf_.data(), sizeof(MsgHeader));
    if (h.magic != kMagic) continue;

    IncomingMessage im;
    im.req_id = h.req_id;
    im.type   = h.type;

    const size_t plen = len - sizeof(MsgHeader);
    im.payload.resize(plen);
    if (plen) {
      std::memcpy(im.payload.data(),
                  self->rx_buf_.data() + sizeof(MsgHeader),
                  plen);
    }

    out.emplace_back(std::move(im));
  }
}

// ---------------------------- lifecycle ----------------------------

UcxTransport::UcxTransport() {
  std::cout << "[UCX] Transport constructor called\n";

  // CRITICAL: initialize all members to known-safe values.
  // This prevents ucp_cleanup()/close()/destroy on garbage pointers/fds.
  ucp_ctx_  = nullptr;
  worker_   = nullptr;
  ep_       = nullptr;
  listener_ = nullptr;

  epoll_fd_ = -1;
  ucx_efd_  = -1;

  is_server_ = false;

  // Default rx buffer (will grow as needed)
  rx_buf_.resize(1024 * 1024);
}

UcxTransport::~UcxTransport() {
  std::lock_guard<std::mutex> lk(mu_);

  if (ep_) {
    ucp_ep_destroy(ep_);
    ep_ = nullptr;
  }
  if (listener_) {
    ucp_listener_destroy(listener_);
    listener_ = nullptr;
  }
  if (worker_) {
    ucp_worker_destroy(worker_);
    worker_ = nullptr;
  }
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
    epoll_fd_ = -1;
  }
  ucx_efd_ = -1;

  if (ucp_ctx_) {
    ucp_cleanup(ucp_ctx_);
    ucp_ctx_ = nullptr;
  }
}

Status UcxTransport::init_ucx_common() {
  std::cout << "[UCX] Initializing UCX common...\n";

  // NOTE: do NOT destroy worker/ep/listener here; do it in create_worker_with_wakeup()
  // because start_server/start_client call init_ucx_common() and then create_worker_with_wakeup().
  if (ucp_ctx_) {
    ucp_cleanup(ucp_ctx_);
    ucp_ctx_ = nullptr;
  }

  ucp_config_t* config = nullptr;
  ucs_status_t st = ucp_config_read(nullptr, nullptr, &config);
  if (st != UCS_OK || !config) {
    return ucx_err("ucp_config_read", st);
  }

  ucp_params_t params;
  std::memset(&params, 0, sizeof(params));

  // Keep field_mask minimal and fully initialized fields only.
  // TAG is required; WAKEUP enables worker_get_efd + worker_arm.
  params.field_mask        = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_MT_WORKERS_SHARED;
  params.features          = UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP;
  params.mt_workers_shared = 1;

  std::cout << "[UCX] Calling ucp_init...\n";
  st = ucp_init(&params, config, &ucp_ctx_);

  ucp_config_release(config);
  config = nullptr;

  if (st != UCS_OK || !ucp_ctx_) {
    ucp_ctx_ = nullptr;
    return ucx_err("ucp_init", st);
  }

  std::cout << "[UCX] UCX context initialized\n";
  return Status::Ok();
}

Status UcxTransport::create_worker_with_wakeup() {
  std::cout << "[UCX] Creating worker...\n";

  if (!ucp_ctx_) return Status::Err("UCX context not initialized");

  // Defensive cleanup (safe even if called more than once)
  if (ep_) { ucp_ep_destroy(ep_); ep_ = nullptr; }
  if (listener_) { ucp_listener_destroy(listener_); listener_ = nullptr; }
  if (worker_) { ucp_worker_destroy(worker_); worker_ = nullptr; }
  if (epoll_fd_ >= 0) { close(epoll_fd_); epoll_fd_ = -1; }
  ucx_efd_ = -1;

  ucp_worker_params_t wparams;
  std::memset(&wparams, 0, sizeof(wparams));
  wparams.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wparams.thread_mode = UCS_THREAD_MODE_MULTI;  // you use send/progress across threads

  ucs_status_t st = ucp_worker_create(ucp_ctx_, &wparams, &worker_);
  if (st != UCS_OK || !worker_) {
    worker_ = nullptr;
    return ucx_err("ucp_worker_create", st);
  }

  std::cout << "[UCX] Worker created successfully\n";

  epoll_fd_ = epoll_create1(0);
  if (epoll_fd_ < 0) {
    return Status::Err(std::string("epoll_create1 failed: ") + std::strerror(errno));
  }

  std::cout << "[UCX] Epoll fd created: " << epoll_fd_ << "\n";

  // WAKEUP integration (optional; may be unsupported on some transports)
  int efd = -1;
  st = ucp_worker_get_efd(worker_, &efd);
  if (st == UCS_OK && efd >= 0) {
    ucx_efd_ = efd;

    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLIN;
    ev.data.fd = ucx_efd_;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ucx_efd_, &ev) < 0) {
      std::cout << "[UCX] Warning: Could not add UCX efd to epoll: " << std::strerror(errno) << "\n";
      ucx_efd_ = -1;
    } else {
      std::cout << "[UCX] UCX event fd registered\n";
    }
  } else {
    std::cout << "[UCX] No UCX event fd (using tcp/self)\n";
  }

  return Status::Ok();
}

// ---------------------------- connection handling ----------------------------

void UcxTransport::on_conn_request(ucp_conn_request_h req, void* arg) {
  std::cout << "[UCX] Connection request callback\n";

  auto* self = reinterpret_cast<UcxTransport*>(arg);
  if (!self) {
    std::cerr << "[UCX] ERROR: Invalid self pointer\n";
    return;
  }

  std::lock_guard<std::mutex> lk(self->mu_);

  if (!self->worker_ || !self->listener_) {
    std::cerr << "[UCX] ERROR: worker/listener not ready, rejecting\n";
    if (self->listener_) ucp_listener_reject(self->listener_, req);
    return;
  }

  if (self->ep_) {
    std::cout << "[UCX] Already have endpoint, rejecting\n";
    ucp_listener_reject(self->listener_, req);
    return;
  }

  ucp_ep_params_t epp;
  std::memset(&epp, 0, sizeof(epp));
  epp.field_mask   = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  epp.conn_request = req;

  ucs_status_t st = ucp_ep_create(self->worker_, &epp, &self->ep_);
  if (st != UCS_OK || !self->ep_) {
    std::cerr << "[UCX] Failed to create endpoint: " << ucs_status_string(st) << "\n";
    self->ep_ = nullptr;
    return;
  }

  std::cout << "[UCX] Endpoint created\n";
}

Status UcxTransport::create_listener() {
  std::cout << "[UCX] Creating listener on " << opt_.listen_host << ":" << opt_.listen_port << "\n";

  if (!worker_) return Status::Err("UCX worker not initialized");

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(opt_.listen_port);

  if (::inet_pton(AF_INET, opt_.listen_host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for: " + opt_.listen_host);
  }

  ucp_listener_params_t lp;
  std::memset(&lp, 0, sizeof(lp));
  lp.field_mask        = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                         UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lp.sockaddr.addr     = (const struct sockaddr*)&addr;
  lp.sockaddr.addrlen  = sizeof(addr);
  lp.conn_handler.cb   = &UcxTransport::on_conn_request;
  lp.conn_handler.arg  = this;

  ucs_status_t st = ucp_listener_create(worker_, &lp, &listener_);
  if (st != UCS_OK || !listener_) {
    listener_ = nullptr;
    return ucx_err("ucp_listener_create", st);
  }

  std::cout << "[UCX] Listener created\n";
  return Status::Ok();
}

Status UcxTransport::connect_to_server() {
  std::cout << "[UCX] Connecting to " << opt_.server_host << ":" << opt_.server_port << "\n";

  if (!worker_) return Status::Err("UCX worker not initialized");

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(opt_.server_port);

  if (::inet_pton(AF_INET, opt_.server_host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for: " + opt_.server_host);
  }

  ucp_ep_params_t epp;
  std::memset(&epp, 0, sizeof(epp));
  epp.field_mask        = UCP_EP_PARAM_FIELD_FLAGS | UCP_EP_PARAM_FIELD_SOCK_ADDR;
  epp.flags             = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  epp.sockaddr.addr     = (const struct sockaddr*)&addr;
  epp.sockaddr.addrlen  = sizeof(addr);

  ucs_status_t st = ucp_ep_create(worker_, &epp, &ep_);
  if (st != UCS_OK || !ep_) {
    ep_ = nullptr;
    return ucx_err("ucp_ep_create(client)", st);
  }

  std::cout << "[UCX] Connected\n";
  return Status::Ok();
}

// ---------------------------- API ----------------------------

Status UcxTransport::start_server(const TransportOptions& opt, MessageHandler on_msg) {
  std::cout << "[UCX] Starting server...\n";

  is_server_ = true;
  opt_       = opt;
  on_msg_    = std::move(on_msg);

  Status s = init_ucx_common();
  if (!s.ok) return s;

  s = create_worker_with_wakeup();
  if (!s.ok) return s;

  s = create_listener();
  if (!s.ok) return s;

  // RX buffer sized in constructor; keep as-is.
  std::cout << "[UCX] Server started\n";
  return Status::Ok();
}

Status UcxTransport::start_client(const TransportOptions& opt, MessageHandler on_msg) {
  std::cout << "[UCX] Starting client...\n";

  is_server_ = false;
  opt_       = opt;
  on_msg_    = std::move(on_msg);

  Status s = init_ucx_common();
  if (!s.ok) return s;

  s = create_worker_with_wakeup();
  if (!s.ok) return s;

  s = connect_to_server();
  if (!s.ok) return s;

  std::cout << "[UCX] Client started\n";
  return Status::Ok();
}

Status UcxTransport::send_bytes(const uint8_t* bytes, size_t len, uint64_t tag) {
  std::lock_guard<std::mutex> lk(mu_);

  if (!worker_) return Status::Err("UCX worker not initialized");
  if (!ep_)     return Status::Err("UCX endpoint not connected");

  ucp_request_param_t param;
  std::memset(&param, 0, sizeof(param));
  param.op_attr_mask = 0;

  void* req = ucp_tag_send_nbx(ep_, bytes, len, tag, &param);
  return ucx_wait(worker_, req);
}

Status UcxTransport::send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) {
  MsgHeader h;
  std::memset(&h, 0, sizeof(h));
  h.magic   = kMagic;
  h.version = kProtoVer;
  h.type    = type;
  h.req_id  = req_id;
  h.flags   = 0;
  h.length  = static_cast<uint32_t>(len);

  std::vector<uint8_t> buf;
  buf.resize(sizeof(MsgHeader) + len);
  std::memcpy(buf.data(), &h, sizeof(MsgHeader));
  if (len) {
    std::memcpy(buf.data() + sizeof(MsgHeader), data, len);
  }

  const uint64_t tag = (type == (uint16_t)MsgType::REQ_INFER) ? TAG_REQ : TAG_RESP;
  return send_bytes(buf.data(), buf.size(), tag);
}

void UcxTransport::pump_recv() {
  // Public-ish wrapper: collect under lock, invoke handler outside lock.
  std::vector<IncomingMessage> msgs;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pump_recv_locked(this, msgs);
  }
  if (on_msg_) {
    for (auto& m : msgs) on_msg_(m);
  }
}

Status UcxTransport::progress(int timeout_ms) {
  if (!worker_) {
    return Status::Err("UCX worker not initialized");
  }

  std::vector<IncomingMessage> msgs;

  {
    // Serialize all UCX worker access across threads.
    std::lock_guard<std::mutex> lk(mu_);

    if (ucx_efd_ >= 0) {
      ucs_status_t arm_st = ucp_worker_arm(worker_);
      // BUSY means there is pending work; we will progress below.
      if (arm_st != UCS_OK && arm_st != UCS_ERR_BUSY) {
        return ucx_err("ucp_worker_arm", arm_st);
      }
    }

    while (ucp_worker_progress(worker_) != 0) {}
    pump_recv_locked(this, msgs);
  }

  // Call handler OUTSIDE mutex to avoid deadlocks (handler may call send()).
  if (on_msg_) {
    for (auto& m : msgs) on_msg_(m);
  }

  // Wait for events (optional)
  if (timeout_ms > 0 && epoll_fd_ >= 0) {
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    int n = epoll_wait(epoll_fd_, &ev, 1, timeout_ms);
    if (n < 0 && errno != EINTR) {
      return Status::Err(std::string("epoll_wait failed: ") + std::strerror(errno));
    }
  }

  return Status::Ok();
}

}  // namespace cc50

#else  // CC50_ENABLE_UCX

namespace cc50 {
UcxTransport::UcxTransport() {}
UcxTransport::~UcxTransport() {}
Status UcxTransport::start_server(const TransportOptions&, MessageHandler) {
  return Status::Err("UCX not enabled");
}
Status UcxTransport::start_client(const TransportOptions&, MessageHandler) {
  return Status::Err("UCX not enabled");
}
Status UcxTransport::send(uint64_t, uint16_t, const uint8_t*, size_t) {
  return Status::Err("UCX not enabled");
}
Status UcxTransport::progress(int) {
  return Status::Err("UCX not enabled");
}
void UcxTransport::pump_recv() {}
}  // namespace cc50

#endif
