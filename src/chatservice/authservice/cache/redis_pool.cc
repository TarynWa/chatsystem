#include "authservice/cache/redis_pool.h"

#include <chrono>
#include <cstring>

namespace chatservice::cache {

RedisPool& RedisPool::instance() {
  static RedisPool pool;
  return pool;
}

RedisPool::~RedisPool() {
  std::lock_guard<std::mutex> lk(mu_);
  for (redisContext* c : idle_) closeConn(c);
  idle_.clear();
}

redisContext* RedisPool::createConn() {
  redisContext* c = redisConnect(host_.c_str(), port_);
  if (!c) return nullptr;
  if (c->err) {
    lastErr_ = c->errstr;
    redisFree(c);
    return nullptr;
  }
  if (db_ > 0) {
    redisReply* r = (redisReply*)redisCommand(c, "SELECT %d", db_);
    if (!r || r->type == REDIS_REPLY_ERROR) {
      if (r) freeReplyObject(r);
      redisFree(c);
      return nullptr;
    }
    freeReplyObject(r);
  }
  return c;
}

void RedisPool::closeConn(redisContext* c) {
  if (c) redisFree(c);
}

bool RedisPool::init(const std::string& host, int port, int db, int minConn,
                     int maxConn) {
  std::lock_guard<std::mutex> lk(mu_);
  if (inited_) return true;
  host_ = host;
  port_ = port;
  db_ = db;
  minConn_ = minConn > 0 ? minConn : 1;
  maxConn_ = maxConn >= minConn_ ? maxConn : minConn_ + 8;

  for (int i = 0; i < minConn_; ++i) {
    redisContext* c = createConn();
    if (!c) {
      lastErr_ = "createConn failed: " + lastErr_;
      return false;
    }
    idle_.push_back(c);
    ++total_;
  }
  inited_ = true;
  return true;
}

redisContext* RedisPool::acquire() {
  std::unique_lock<std::mutex> lk(mu_);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    while (!idle_.empty()) {
      redisContext* c = idle_.front();
      idle_.pop_front();
      // 复用前 PING 探活; 失效则销毁重建
      redisReply* r = (redisReply*)redisCommand(c, "PING");
      bool ok = r && r->type == REDIS_REPLY_STATUS &&
                strncmp(r->str, "PONG", 4) == 0;
      if (r) freeReplyObject(r);
      if (ok) return c;
      --total_;
      closeConn(c);
    }
    if (total_ < maxConn_) {
      redisContext* c = createConn();
      if (c) {
        ++total_;
        return c;
      }
    }
    if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
      if (total_ < maxConn_) {
        redisContext* c = createConn();
        if (c) {
          ++total_;
          return c;
        }
      }
      return nullptr;
    }
  }
}

void RedisPool::release(redisContext* c) {
  if (!c) return;
  std::lock_guard<std::mutex> lk(mu_);
  idle_.push_back(c);
  cv_.notify_one();
}

redisReply* RedisPool::run(redisContext* c,
                           const std::vector<std::string>& args) {
  if (!c || args.empty()) return nullptr;
  std::vector<const char*> argv;
  std::vector<size_t> argvlen;
  argv.reserve(args.size());
  argvlen.reserve(args.size());
  for (const auto& a : args) {
    argv.push_back(a.data());
    argvlen.push_back(a.size());
  }
  return (redisReply*)redisCommandArgv(c, (int)args.size(), argv.data(),
                                       argvlen.data());
}

} // namespace chatservice::cache
