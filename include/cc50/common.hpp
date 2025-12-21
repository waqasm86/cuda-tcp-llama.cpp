#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <stdexcept>
#include <iostream>
#include <chrono>

namespace cc50 {

static constexpr uint32_t kMagic = 0x30354343u; // 'CC50'

inline uint64_t now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Status {
  bool ok{true};
  std::string msg{};
  static Status Ok() { return {true, {}}; }
  static Status Err(std::string m) { return {false, std::move(m)}; }
};

inline void die(const std::string& m) {
  throw std::runtime_error(m);
}

} // namespace cc50
