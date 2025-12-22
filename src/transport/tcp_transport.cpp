#include "cc50/transport/tcp_transport.hpp"
#include "cc50/protocol.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace cc50 {

namespace {
constexpr int kListenBacklog = 16;
constexpr size_t kReadChunk  = 4096;

int make_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

Status add_epoll_fd(int ep, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events  = events;
  ev.data.fd = fd;
  if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
    return Status::Err(std::string("epoll_ctl add failed: ") + std::strerror(errno));
  }
  return Status::Ok();
}

Status mod_epoll_fd(int ep, int fd, uint32_t events) {
  epoll_event ev{};
  ev.events  = events;
  ev.data.fd = fd;
  if (epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev) < 0) {
    return Status::Err(std::string("epoll_ctl mod failed: ") + std::strerror(errno));
  }
  return Status::Ok();
}

}  // namespace

TcpTransport::TcpTransport() {
  ep_ = epoll_create1(0);
}

TcpTransport::~TcpTransport() {
  for (auto& [fd, conn] : conns_) {
    (void)conn;
    close(fd);
  }
  conns_.clear();

  if (listen_fd_ >= 0) close(listen_fd_);
  if (peer_fd_ >= 0 && conns_.count(peer_fd_) == 0) close(peer_fd_);
  if (ep_ >= 0) close(ep_);
}

Status TcpTransport::make_listen_socket(const std::string& host, uint16_t port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return Status::Err(std::string("socket failed: ") + std::strerror(errno));
  }

  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for: " + host);
  }

  if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
    return Status::Err(std::string("bind failed: ") + std::strerror(errno));
  }
  if (::listen(listen_fd_, kListenBacklog) < 0) {
    return Status::Err(std::string("listen failed: ") + std::strerror(errno));
  }
  if (make_non_blocking(listen_fd_) < 0) {
    return Status::Err(std::string("failed to set non-blocking: ") + std::strerror(errno));
  }

  return add_epoll_fd(ep_, listen_fd_, EPOLLIN);
}

Status TcpTransport::accept_new() {
  while (true) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int cfd = ::accept4(listen_fd_, (sockaddr*)&addr, &len, SOCK_NONBLOCK);
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::Ok();
      return Status::Err(std::string("accept failed: ") + std::strerror(errno));
    }

    Conn c{};
    c.fd = cfd;
    conns_.emplace(cfd, std::move(c));
    if (peer_fd_ < 0) peer_fd_ = cfd;

    auto st = add_epoll_fd(ep_, cfd, EPOLLIN | EPOLLRDHUP);
    if (!st.ok) return st;
  }
}

Status TcpTransport::queue_send(int fd, const uint8_t* bytes, size_t len) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return Status::Err("peer not connected");

  auto& c = it->second;
  c.tx.insert(c.tx.end(), bytes, bytes + len);

  uint32_t events = EPOLLIN | EPOLLRDHUP;
  if (!c.tx.empty()) events |= EPOLLOUT;
  return mod_epoll_fd(ep_, fd, events);
}

Status TcpTransport::handle_write(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return Status::Err("peer not connected");
  auto& c = it->second;

  while (c.tx_off < c.tx.size()) {
    ssize_t n = ::send(fd, c.tx.data() + c.tx_off, c.tx.size() - c.tx_off, 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return Status::Err(std::string("send failed: ") + std::strerror(errno));
    }
    if (n == 0) break;
    c.tx_off += (size_t)n;
  }

  if (c.tx_off >= c.tx.size()) {
    c.tx.clear();
    c.tx_off = 0;
    return mod_epoll_fd(ep_, fd, EPOLLIN | EPOLLRDHUP);
  }

  return Status::Ok();
}

Status TcpTransport::handle_read(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return Status::Err("peer not connected");
  auto& c = it->second;

  uint8_t buf[kReadChunk];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return Status::Err(std::string("recv failed: ") + std::strerror(errno));
    }
    if (n == 0) {
      // peer closed
      ::close(fd);
      conns_.erase(it);
      if (peer_fd_ == fd) peer_fd_ = -1;
      return Status::Err("peer closed");
    }

    c.rx.insert(c.rx.end(), buf, buf + n);

    while (c.rx.size() >= sizeof(MsgHeader)) {
      MsgHeader h{};
      std::memcpy(&h, c.rx.data(), sizeof(h));
      if (h.magic != kMagic) return Status::Err("bad magic");

      size_t need = sizeof(MsgHeader) + h.length;
      if (c.rx.size() < need) break;

      IncomingMessage msg{};
      msg.req_id = h.req_id;
      msg.type   = h.type;
      if (h.length) {
        msg.payload.resize(h.length);
        std::memcpy(msg.payload.data(), c.rx.data() + sizeof(MsgHeader), h.length);
      }

      if (on_msg_) on_msg_(msg);

      c.rx.erase(c.rx.begin(), c.rx.begin() + (long)need);
    }
  }

  return Status::Ok();
}

Status TcpTransport::start_server(const TransportOptions& opt, MessageHandler on_msg) {
  if (ep_ < 0) return Status::Err("epoll not available");
  is_server_ = true;
  opt_       = opt;
  on_msg_    = std::move(on_msg);

  return make_listen_socket(opt.listen_host, opt.listen_port);
}

Status TcpTransport::start_client(const TransportOptions& opt, MessageHandler on_msg) {
  if (ep_ < 0) return Status::Err("epoll not available");
  is_server_ = false;
  opt_       = opt;
  on_msg_    = std::move(on_msg);

  peer_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (peer_fd_ < 0) {
    return Status::Err(std::string("socket failed: ") + std::strerror(errno));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(opt.server_port);
  if (::inet_pton(AF_INET, opt.server_host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for: " + opt.server_host);
  }

  if (::connect(peer_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
    return Status::Err(std::string("connect failed: ") + std::strerror(errno));
  }
  if (make_non_blocking(peer_fd_) < 0) {
    return Status::Err(std::string("failed to set non-blocking: ") + std::strerror(errno));
  }

  Conn c{};
  c.fd = peer_fd_;
  conns_.emplace(peer_fd_, std::move(c));

  return add_epoll_fd(ep_, peer_fd_, EPOLLIN | EPOLLRDHUP);
}

Status TcpTransport::send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) {
  MsgHeader h{};
  h.magic   = kMagic;
  h.version = kProtoVer;
  h.type    = type;
  h.req_id  = req_id;
  h.flags   = 0;
  h.length  = static_cast<uint32_t>(len);

  std::vector<uint8_t> buf;
  buf.resize(sizeof(MsgHeader) + len);
  std::memcpy(buf.data(), &h, sizeof(MsgHeader));
  if (len) std::memcpy(buf.data() + sizeof(MsgHeader), data, len);

  int fd = peer_fd_;
  if (is_server_ && fd < 0 && !conns_.empty()) {
    fd = conns_.begin()->first;
  }
  if (fd < 0) return Status::Err("no peer connected");

  return queue_send(fd, buf.data(), buf.size());
}

Status TcpTransport::progress(int timeout_ms) {
  if (ep_ < 0) return Status::Err("epoll not available");

  std::vector<epoll_event> events(opt_.epoll_max_events);
  int n = epoll_wait(ep_, events.data(), (int)events.size(), timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return Status::Ok();
    return Status::Err(std::string("epoll_wait failed: ") + std::strerror(errno));
  }

  for (int i = 0; i < n; i++) {
    int fd = events[i].data.fd;
    uint32_t ev = events[i].events;

    if (fd == listen_fd_) {
      auto st = accept_new();
      if (!st.ok) return st;
      continue;
    }

    if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
      ::close(fd);
      conns_.erase(fd);
      if (peer_fd_ == fd) peer_fd_ = -1;
      continue;
    }

    if (ev & EPOLLIN) {
      auto st = handle_read(fd);
      if (!st.ok) return st;
    }
    if (ev & EPOLLOUT) {
      auto st = handle_write(fd);
      if (!st.ok) return st;
    }
  }

  return Status::Ok();
}

}  // namespace cc50
