// 自研 Redis 连接池(hiredis, 线程安全)。连接只借给单次命令序列, 用完即还。
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <hiredis/hiredis.h>

namespace chatservice::cache {

class RedisPool {
 public:
  static RedisPool& instance();

  bool init(const std::string& host, int port, int db, int minConn, int maxConn);
  bool inited() const { return inited_; }

  // 借连接(至多等 5s); 失败返回 nullptr
  redisContext* acquire();
  void release(redisContext* c);

  // 执行一条命令(逐参数传入, 免去转义/注入); 返回 reply, 调用方 freeReplyObject
  static redisReply* run(redisContext* c, const std::vector<std::string>& args);

  std::string lastErr() const { return lastErr_; }

 private:
  RedisPool() = default;
  ~RedisPool();
  RedisPool(const RedisPool&) = delete;
  RedisPool& operator=(const RedisPool&) = delete;

  redisContext* createConn();
  void closeConn(redisContext* c);

  std::string host_;
  int port_ = 6379, db_ = 0;
  int minConn_ = 1, maxConn_ = 4;
  bool inited_ = false;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<redisContext*> idle_;
  int total_ = 0;
  std::string lastErr_;
};

// 连接 RAII
struct RedisConn {
  redisContext* c;
  RedisConn() : c(RedisPool::instance().acquire()) {}
  ~RedisConn() { RedisPool::instance().release(c); }
  bool ok() const { return c != nullptr; }
};

} // namespace chatservice::cache
