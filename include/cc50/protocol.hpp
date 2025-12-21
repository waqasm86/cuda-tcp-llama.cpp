#pragma once
#include <cstdint>

namespace cc50 {

enum class MsgType : uint16_t {
  REQ_INFER   = 1,
  RESP_CHUNK = 2,
  RESP_DONE  = 3,
  RESP_ERR   = 4,
};

struct MsgHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint64_t req_id;
  uint32_t flags;
  uint32_t length;
};

static constexpr uint16_t kProtoVer = 1;

struct InferRequestHdr {
  uint32_t max_tokens;
  uint32_t credit_bytes;
  uint32_t prompt_len;
  // prompt bytes follow
};

struct InferDone {
  uint32_t tokens;
  uint32_t reserved;
  uint64_t elapsed_us;
};

struct ErrMsg {
  uint32_t msg_len;
  // msg bytes follow
};

} // namespace cc50
