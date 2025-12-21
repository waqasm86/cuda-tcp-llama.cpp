#include "cc50/transport/tcp_transport.hpp"
#include "cc50/protocol.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace cc50 {

static Status set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return Status::Err("fcntl(F_GETFL) failed");
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return Status::Err("fcntl(F_SETFL) failed");
  return Status::Ok();
}

TcpTransport::TcpTransport() {}
TcpTransport::~TcpTransport() {
  if (listen_fd_ >= 0) close(listen_fd_);
  if (peer_fd_ >= 0) close(peer_fd_);
  if (ep_ >= 0) close(ep_);
  for (auto& [fd, c] : conns_) {
    if (fd >= 0) close(fd);
  }
}

Status TcpTransport::make_listen_socket(const std::string& host, uint16_t port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return Status::Err("socket() failed");

  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for listen host");
  }

  if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
    return Status::Err(std::string("bind failed: ") + strerror(errno));
  }
  if (::listen(listen_fd_, 128) < 0) return Status::Err("listen failed");
  auto s = set_nonblock(listen_fd_);
  if (!s.ok) return s;
  return Status::Ok();
}

Status TcpTransport::start_server(const TransportOptions& opt, MessageHandler on_msg) {
  is_server_ = true;
  opt_ = opt;
  on_msg_ = std::move(on_msg);

  ep_ = epoll_create1(0);
  if (ep_ < 0) return Status::Err("epoll_create1 failed");

  auto s = make_listen_socket(opt.listen_host, opt.listen_port);
  if (!s.ok) return s;

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd_;
  if (epoll_ctl(ep_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) return Status::Err("epoll_ctl add listen failed");
  return Status::Ok();
}

Status TcpTransport::start_client(const TransportOptions& opt, MessageHandler on_msg) {
  is_server_ = false;
  opt_ = opt;
  on_msg_ = std::move(on_msg);

  ep_ = epoll_create1(0);
  if (ep_ < 0) return Status::Err("epoll_create1 failed");

  peer_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (peer_fd_ < 0) return Status::Err("socket() failed");
  set_nonblock(peer_fd_);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt.server_port);
  if (::inet_pton(AF_INET, opt.server_host.c_str(), &addr.sin_addr) != 1) {
    return Status::Err("inet_pton failed for server host");
  }

  int rc = ::connect(peer_fd_, (sockaddr*)&addr, sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    return Status::Err(std::string("connect failed: ") + strerror(errno));
  }

  Conn c{};
  c.fd = peer_fd_;
  conns_.emplace(peer_fd_, std::move(c));

  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLOUT;
  ev.data.fd = peer_fd_;
  if (epoll_ctl(ep_, EPOLL_CTL_ADD, peer_fd_, &ev) < 0) return Status::Err("epoll_ctl add peer failed");
  return Status::Ok();
}

Status TcpTransport::accept_new() {
  while (true) {
    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    int fd = ::accept(listen_fd_, (sockaddr*)&addr, &alen);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return Status::Err("accept failed");
    }
    set_nonblock(fd);
    Conn c{};
    c.fd = fd;
    conns_.emplace(fd, std::move(c));
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = fd;
    epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
  }
  return Status::Ok();
}

static void append_u32(std::vector<uint8_t>& v, uint32_t x) {
  uint8_t b[4];
  std::memcpy(b, &x, 4);
  v.insert(v.end(), b, b+4);
}

Status TcpTransport::queue_send(int fd, const uint8_t* bytes, size_t len) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return Status::Err("unknown fd");
  auto& c = it->second;
  c.tx.insert(c.tx.end(), bytes, bytes + len);
  return Status::Ok();
}

Status TcpTransport::send(uint64_t req_id, uint16_t type, const uint8_t* data, size_t len) {
  // For simplicity:
  // - client: send to peer_fd_
  // - server: broadcast to all conns (demo)
  MsgHeader h{};
  h.magic = kMagic;
  h.version = kProtoVer;
  h.type = type;
  h.req_id = req_id;
  h.flags = 0;
  h.length = static_cast<uint32_t>(len);

  std::vector<uint8_t> frame;
  frame.reserve(4 + sizeof(MsgHeader) + len);

  uint32_t total = static_cast<uint32_t>(sizeof(MsgHeader) + len);
  append_u32(frame, total);
  frame.insert(frame.end(), (uint8_t*)&h, (uint8_t*)&h + sizeof(MsgHeader));
  if (len) frame.insert(frame.end(), data, data + len);

  if (!is_server_) {
    return queue_send(peer_fd_, frame.data(), frame.size());
  } else {
    for (auto& [fd, c] : conns_) {
      if (fd == listen_fd_) continue;
      queue_send(fd, frame.data(), frame.size());
    }
    return Status::Ok();
  }
}

Status TcpTransport::handle_read(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return Status::Ok();
  auto& c = it->second;

  uint8_t tmp[8192];
  while (true) {
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return Status::Err("read failed");
    }
    if (n == 0) {
      // peer closed
      close(fd);
      conns_.erase(fd);
      break;
    }
    c.rx.insert(c.rx.end(), tmp, tmp + n);

    // parse frames: [u32 total][MsgHeader][payload]
    while (c.rx.size() >= 4) {
      uint32_t total{};
      std::memcpy(&total, c.rx.data(), 4);
      if (c.rx.size() < 4 + total) break;
      if (total < sizeof(MsgHeader)) return Status::Err("bad frame");
      MsgHeader h{};
      std::memcpy(&h, c.rx.data() + 4, sizeof(MsgHeader));
      if (h.magic != kMagic) return Status::Err("bad magic");
      size_t plen = total - sizeof(MsgHeader);
      IncomingMessage im{};
      im.req_id = h.req_id;
      im.type = h.type;
      im.payload.resize(plen);
      if (plen) std::memcpy(im.payload.data(), c.rx.data() + 4 + sizeof(MsgHeader), plen);
      if (on_msg_) on_msg_(im);
      c.rx.erase(c.rx.begin(), c.rx.begin() + 4 + total);
    }
  }
  return Status::Ok();
}

Status TcpTransport::handle_write(int fd) {
  auto it = conns_.find(fd);
  if (it == conns_.end()) return Status::Ok();
  auto& c = it->second;

  while (c.tx_off < c.tx.size()) {
    ssize_t n = ::write(fd, c.tx.data() + c.tx_off, c.tx.size() - c.tx_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return Status::Err("write failed");
    }
    c.tx_off += static_cast<size_t>(n);
  }
  if (c.tx_off == c.tx.size()) {
    c.tx.clear();
    c.tx_off = 0;
  }
  return Status::Ok();
}

Status TcpTransport::progress(int timeout_ms) {
  epoll_event evs[256];
  int n = epoll_wait(ep_, evs, 256, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return Status::Ok();
    return Status::Err("epoll_wait failed");
  }
  for (int i = 0; i < n; i++) {
    int fd = evs[i].data.fd;
    if (is_server_ && fd == listen_fd_) {
      auto s = accept_new();
      if (!s.ok) return s;
      continue;
    }
    if (evs[i].events & EPOLLIN) {
      auto s = handle_read(fd);
      if (!s.ok) return s;
    }
    if (evs[i].events & EPOLLOUT) {
      auto s = handle_write(fd);
      if (!s.ok) return s;
    }
  }
  return Status::Ok();
}

} // namespace cc50
