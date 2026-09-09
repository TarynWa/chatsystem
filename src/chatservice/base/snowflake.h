// 线程安全雪花ID(S0 单机: worker 固定, 进程内唯一)
// 位布局: 41bit 毫秒(epoch 2024-01-01) | 10bit worker | 12bit 序列号
// msg_id(S0 单聊消息) 与后续 group_id 共用; 跨进程 worker 需不同(分布式取号属远期)。
#pragma once
#include <cstdint>
#include <mutex>

namespace chatservice::common {

class Snowflake {
 public:
  static Snowflake& instance();
  void init(int workerId);  // main 启动调一次; 默认 1
  uint64_t nextId();        // 返回 >0 递增 id; 时钟回拨时自旋等追平

 private:
  Snowflake() = default;
  static constexpr uint64_t kEpochMs = 1704067200000ULL;  // 2024-01-01T00:00:00Z

  std::mutex mu_;
  uint64_t lastMs_ = 0;
  uint16_t seq_ = 0;
  int workerId_ = 1;
};

} // namespace chatservice::common
