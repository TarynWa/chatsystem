#include "base/snowflake.h"

#include <chrono>
#include <thread>

namespace chatservice::common {
namespace {

uint64_t nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

Snowflake& Snowflake::instance() {
  static Snowflake s;
  return s;
}

void Snowflake::init(int workerId) {
  std::lock_guard<std::mutex> lk(mu_);
  if (workerId >= 0 && workerId < 1024) workerId_ = workerId;
}

uint64_t Snowflake::nextId() {
  std::lock_guard<std::mutex> lk(mu_);
  uint64_t ts = nowMs();
  if (ts < lastMs_) {  // 时钟回拨: 阻塞追平, 保证不回退
    while (nowMs() < lastMs_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ts = nowMs();
  }
  if (ts == lastMs_) {
    seq_ = static_cast<uint16_t>((seq_ + 1) & 0xFFF);
    if (seq_ == 0) {  // 同毫秒 4096 个耗尽: 等下一毫秒
      while (nowMs() <= ts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      ts = nowMs();
    }
  } else {
    seq_ = 0;
  }
  lastMs_ = ts;
  const uint64_t rel = ts - kEpochMs;
  return (rel << 22) | (static_cast<uint64_t>(workerId_ & 0x3FF) << 12) | seq_;
}

} // namespace chatservice::common
