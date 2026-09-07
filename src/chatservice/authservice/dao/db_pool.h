// 自研 MySQL 连接池（mysqlclient C API, 线程安全）
// 连接只借给单次请求，用完即还；不跨请求共享。
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <mysql/mysql.h>

namespace chatservice::dao {

class DbPool {
 public:
  static DbPool& instance();

  // 只允许初始化一次；失败返回 false（多为配置/网络问题）
  bool init(const std::string& host, unsigned int port,
            const std::string& user, const std::string& passwd,
            const std::string& db, int minConn, int maxConn,
            unsigned int timeoutMs = 5000);

  bool inited() const { return inited_; }

  // 借连接（阻塞至 timeoutMs）；返回 nullptr 表示超时
  MYSQL* acquire();
  void release(MYSQL* conn);

  // SQL 注入转义（须持有连接时调用）
  std::string escape(MYSQL* conn, const std::string& s);

  std::string lastErr() const { return lastErr_; }

 private:
  DbPool() = default;
  ~DbPool();
  DbPool(const DbPool&) = delete;
  DbPool& operator=(const DbPool&) = delete;

  MYSQL* createConn();
  void closeConn(MYSQL* c);

  std::string host_, user_, passwd_, db_;
  unsigned int port_ = 0;
  int minConn_ = 1, maxConn_ = 4;
  bool inited_ = false;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<MYSQL*> idle_;
  int total_ = 0;  // 已创建连接数
  std::string lastErr_;
};

// 连接 RAII：出作用域自动还池（借一个、用一次、立刻还）
struct DbConn {
  MYSQL* c;
  DbConn() : c(DbPool::instance().acquire()) {}
  ~DbConn() { DbPool::instance().release(c); }
  bool ok() const { return c != nullptr; }
};

} // namespace chatservice::dao
