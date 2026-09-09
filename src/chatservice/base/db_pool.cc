#include "db_pool.h"

#include <chrono>
#include <cstring>

namespace chatservice::dao {

DbPool& DbPool::instance() {
  static DbPool pool;
  return pool;
}

DbPool::~DbPool() {
  std::lock_guard<std::mutex> lk(mu_);
  for (MYSQL* c : idle_) closeConn(c);
  idle_.clear();
}

MYSQL* DbPool::createConn() {
  MYSQL* c = mysql_init(nullptr);
  if (!c) return nullptr;
  // host=="localhost" 走 unix socket(与 mysql CLI 一致，可吃本地 socket auth)
  if (!mysql_real_connect(c, host_.c_str(), user_.c_str(), passwd_.c_str(),
                          db_.c_str(), port_, nullptr, 0)) {
    lastErr_ = mysql_error(c);
    mysql_close(c);
    return nullptr;
  }
  return c;
}

void DbPool::closeConn(MYSQL* c) {
  if (c) mysql_close(c);
}

bool DbPool::init(const std::string& host, unsigned int port,
                  const std::string& user, const std::string& passwd,
                  const std::string& db, int minConn, int maxConn,
                  unsigned int timeoutMs) {
  std::lock_guard<std::mutex> lk(mu_);
  if (inited_) return true;
  host_ = host; port_ = port; user_ = user; passwd_ = passwd; db_ = db;
  minConn_ = minConn > 0 ? minConn : 1;
  maxConn_ = maxConn >= minConn_ ? maxConn : minConn_ + 8;

  for (int i = 0; i < minConn_; ++i) {
    MYSQL* c = createConn();
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

MYSQL* DbPool::acquire() {
  std::unique_lock<std::mutex> lk(mu_);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    while (!idle_.empty()) {
      MYSQL* c = idle_.front();
      idle_.pop_front();
      if (mysql_ping(c) == 0) return c;  // 连接健康，直接可用
      --total_;
      closeConn(c);  // 断线则销毁，继续看下一个/新建
    }
    if (total_ < maxConn_) {
      MYSQL* c = createConn();
      if (c) {
        ++total_;
        return c;
      }
      // 新建失败(如瞬时/连接数压力)，转入等待归还
    }
    if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
      // 等待超时：兜底再尝试一次新建，仍无则放弃
      if (total_ < maxConn_) {
        MYSQL* c = createConn();
        if (c) {
          ++total_;
          return c;
        }
      }
      return nullptr;
    }
  }
}

void DbPool::release(MYSQL* conn) {
  if (!conn) return;
  std::lock_guard<std::mutex> lk(mu_);
  idle_.push_back(conn);
  cv_.notify_one();
}

std::string DbPool::escape(MYSQL* conn, const std::string& s) {
  if (!conn) return "";
  std::string out(s.size() * 2 + 1, '\0');
  unsigned long n = mysql_real_escape_string(conn, &out[0], s.data(), s.size());
  out.resize(n);
  return out;
}

} // namespace chatservice::dao
