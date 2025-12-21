#include "cc50/backend/llama_server_backend.hpp"
#include "cc50/common.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace cc50 {

static std::string ltrim_copy(std::string s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c){ return !std::isspace(c); }));
  return s;
}

static std::string join_paths(std::string a, std::string b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (a.back() == '/' && b.front() == '/') return a + b.substr(1);
  if (a.back() != '/' && b.front() != '/') return a + "/" + b;
  return a + b;
}

struct UrlParts {
  std::string host;
  int port{80};
  std::string path{"/"};
};

static bool parse_http_url(const std::string& base_url, const std::string& endpoint, UrlParts& out, std::string& err) {
  std::string url = base_url;
  if (url.rfind("http://", 0) == 0) url = url.substr(7);
  else if (url.rfind("https://", 0) == 0) { err = "https:// not supported (use http://)"; return false; }

  // Split path from host[:port]
  std::string hostport = url;
  std::string base_path;
  auto slash = url.find('/');
  if (slash != std::string::npos) {
    hostport = url.substr(0, slash);
    base_path = url.substr(slash); // includes '/'
  }

  std::string host = hostport;
  int port = 80;
  auto colon = hostport.rfind(':');
  if (colon != std::string::npos && colon + 1 < hostport.size()) {
    host = hostport.substr(0, colon);
    port = std::atoi(hostport.substr(colon + 1).c_str());
    if (port <= 0) { err = "invalid port in url"; return false; }
  }

  out.host = host;
  out.port = port;
  out.path = join_paths(base_path.empty() ? std::string("/") : base_path, endpoint.empty() ? std::string("/") : endpoint);
  if (out.path.empty() || out.path[0] != '/') out.path = "/" + out.path;
  return true;
}

static bool set_socket_timeout(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return false;
  return true;
}

static Status http_post_json(const UrlParts& u, int connect_timeout_ms, int request_timeout_ms,
                             const std::string& body, int& http_status, std::string& out_body) {
  http_status = 0;
  out_body.clear();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* res = nullptr;
  std::string port_str = std::to_string(u.port);
  int gai = getaddrinfo(u.host.c_str(), port_str.c_str(), &hints, &res);
  if (gai != 0) return Status::Err(std::string("getaddrinfo: ") + gai_strerror(gai));

  int fd = -1;
  for (addrinfo* p = res; p; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;

    // connect with a reasonable timeout by relying on SO_SNDTIMEO (simpler than non-blocking+select here)
    set_socket_timeout(fd, connect_timeout_ms);

    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return Status::Err(std::string("connect failed: ") + std::strerror(errno));

  set_socket_timeout(fd, request_timeout_ms);

  std::string req;
  req.reserve(512 + body.size());
  req += "POST " + u.path + " HTTP/1.1\r\n";
  req += "Host: " + u.host + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Accept: application/json\r\n";
  req += "Connection: close\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  req += body;

  const char* ptr = req.data();
  size_t left = req.size();
  while (left > 0) {
    ssize_t n = ::send(fd, ptr, left, 0);
    if (n < 0) { ::close(fd); return Status::Err(std::string("send failed: ") + std::strerror(errno)); }
    ptr += n;
    left -= (size_t)n;
  }

  std::string resp;
  char buf[8192];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break;
    if (n < 0) { ::close(fd); return Status::Err(std::string("recv failed: ") + std::strerror(errno)); }
    resp.append(buf, buf + n);
  }
  ::close(fd);

  // Split headers/body
  auto sep = resp.find("\r\n\r\n");
  if (sep == std::string::npos) return Status::Err("bad http response (no header separator)");
  std::string headers = resp.substr(0, sep);
  std::string body_part = resp.substr(sep + 4);

  // Parse status line: HTTP/1.1 200 OK
  auto eol = headers.find("\r\n");
  std::string status_line = (eol == std::string::npos) ? headers : headers.substr(0, eol);
  {
    auto sp1 = status_line.find(' ');
    auto sp2 = (sp1 == std::string::npos) ? std::string::npos : status_line.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      http_status = std::atoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    }
  }

  // Check for chunked encoding
  bool chunked = false;
  {
    std::string hl = headers;
    std::transform(hl.begin(), hl.end(), hl.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (hl.find("transfer-encoding: chunked") != std::string::npos) chunked = true;
  }

  if (!chunked) {
    out_body = body_part;
    return Status::Ok();
  }

  // Decode chunked
  std::string decoded;
  size_t i = 0;
  while (i < body_part.size()) {
    // read chunk size line
    size_t line_end = body_part.find("\r\n", i);
    if (line_end == std::string::npos) break;
    std::string sz_hex = body_part.substr(i, line_end - i);
    size_t semi = sz_hex.find(';');
    if (semi != std::string::npos) sz_hex = sz_hex.substr(0, semi);
    int sz = std::strtol(sz_hex.c_str(), nullptr, 16);
    i = line_end + 2;
    if (sz <= 0) break;
    if (i + (size_t)sz > body_part.size()) break;
    decoded.append(body_part.data() + i, (size_t)sz);
    i += (size_t)sz;
    // skip trailing CRLF
    if (i + 2 <= body_part.size() && body_part.substr(i,2) == "\r\n") i += 2;
  }
  out_body = decoded;
  return Status::Ok();
}

Status LlamaServerBackend::init() {
  return Status::Ok();
}

Status LlamaServerBackend::load_model(const std::string&, int, int) {
  // no-op: llama-server already has the model loaded.
  return Status::Ok();
}

std::string LlamaServerBackend::json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 32);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else out += c;
    }
  }
  return out;
}

static bool json_read_string_at(const std::string& s, size_t pos, std::string& out) {
  // pos points at opening quote
  if (pos >= s.size() || s[pos] != '"') return false;
  pos++;
  std::string r;
  while (pos < s.size()) {
    char c = s[pos++];
    if (c == '"') { out = r; return true; }
    if (c == '\\' && pos < s.size()) {
      char e = s[pos++];
      switch (e) {
        case '"': r.push_back('"'); break;
        case '\\': r.push_back('\\'); break;
        case '/': r.push_back('/'); break;
        case 'b': r.push_back('\b'); break;
        case 'f': r.push_back('\f'); break;
        case 'n': r.push_back('\n'); break;
        case 'r': r.push_back('\r'); break;
        case 't': r.push_back('\t'); break;
        case 'u': {
          // minimal \uXXXX (BMP) support: decode hex to UTF-8 if <= 0x7F else keep as '?'
          if (pos + 4 <= s.size()) {
            unsigned int v = 0;
            for (int i = 0; i < 4; i++) {
              char h = s[pos + i];
              v <<= 4;
              if (h >= '0' && h <= '9') v |= (h - '0');
              else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
              else { v = 0; break; }
            }
            pos += 4;
            if (v <= 0x7F) r.push_back((char)v);
            else r.push_back('?');
          }
        } break;
        default: r.push_back(e); break;
      }
    } else {
      r.push_back(c);
    }
  }
  return false;
}

bool LlamaServerBackend::json_extract_string(const std::string& body, const char* key, std::string& out) {
  std::string k = "\"";
  k += key;
  k += "\"";
  size_t p = body.find(k);
  if (p == std::string::npos) return false;
  p = body.find(':', p + k.size());
  if (p == std::string::npos) return false;
  p++;
  while (p < body.size() && std::isspace((unsigned char)body[p])) p++;
  if (p >= body.size() || body[p] != '"') return false;
  return json_read_string_at(body, p, out);
}

Status LlamaServerBackend::infer_stream(const InferRequest& req, StreamFn on_chunk, InferResult& out) {
  const uint64_t t0 = now_us();
  out = InferResult{};

  auto call = [&](const std::string& endpoint, const std::string& body, std::string& text_out)->Status {
    UrlParts u;
    std::string err;
    if (!parse_http_url(opt_.base_url, endpoint, u, err)) return Status::Err("parse url: " + err);

    int status = 0;
    std::string resp_body;
    auto st = http_post_json(u, opt_.connect_timeout_ms, opt_.request_timeout_ms, body, status, resp_body);
    if (!st.ok) return st;
    if (status < 200 || status >= 300) {
      return Status::Err("llama-server http status=" + std::to_string(status) + " body=" + resp_body.substr(0, 200));
    }

    // Try common fields.
    std::string tmp;
    if (json_extract_string(resp_body, "content", tmp) ||
        json_extract_string(resp_body, "response", tmp) ||
        json_extract_string(resp_body, "completion", tmp) ||
        json_extract_string(resp_body, "text", tmp)) {
      text_out = std::move(tmp);
      return Status::Ok();
    }

    // OpenAI-style: choices[0].text (we just search for the first "text":"...")
    if (json_extract_string(resp_body, "text", tmp)) {
      text_out = std::move(tmp);
      return Status::Ok();
    }

    return Status::Err("could not parse completion text from response (unexpected schema)");
  };

  // Primary attempt: /completion (llama.cpp classic)
  std::string text;
  {
    std::string body = "{";
    body += "\"prompt\":\"" + json_escape(req.prompt) + "\",";
    body += "\"n_predict\":" + std::to_string(req.max_tokens) + ",";
    body += "\"stream\":false";
    body += "}";

    auto st = call(opt_.endpoint, body, text);
    if (!st.ok) {
      // Fallback: /v1/completions
      std::string body2 = "{";
      body2 += "\"model\":\"\",";
      body2 += "\"prompt\":\"" + json_escape(req.prompt) + "\",";
      body2 += "\"max_tokens\":" + std::to_string(req.max_tokens) + ",";
      body2 += "\"stream\":false";
      body2 += "}";
      auto st2 = call("/v1/completions", body2, text);
      if (!st2.ok) {
        out.error = st.msg + " | fallback: " + st2.msg;
        return Status::Err(out.error);
      }
    }
  }

  // Re-chunk into RESP_CHUNK messages to mimic streaming.
  out.text = text;
  out.tokens = 0; // token count unknown without server-provided metadata
  for (size_t i = 0; i < text.size(); i += opt_.chunk_bytes) {
    std::string_view sv(text.data() + i, std::min(opt_.chunk_bytes, text.size() - i));
    if (on_chunk) on_chunk(std::string(sv));
  }

  out.elapsed_us = now_us() - t0;
  return Status::Ok();
}

} // namespace cc50
