// SQL 小工具：转义 + 拼串 + 事务（S0 用字符串拼 SQL + 转义防注入；
// README §5.5 目标为 mysql_stmt_* 预处理语句，M4 统一替换）
// 由 authservice/dao/sqlutil.h 迁入 chatservice 顶层共享(S0 msg/social 复用)。
#pragma once
#include <mysql/mysql.h>
#include <string>

namespace chatservice::dao {

// 转义一段字符串（调用方须持有连接）
inline std::string esc(MYSQL* c, const std::string& s) {
  if (!c) return "";
  std::string out(s.size() * 2 + 1, '\0');
  unsigned long n = mysql_real_escape_string(c, &out[0], s.data(), s.size());
  out.resize(n);
  return out;
}

// 包单引号（已转义）
inline std::string q(MYSQL* c, const std::string& s) {
  return "'" + esc(c, s) + "'";
}

// 可选字段(邮箱/手机): 空串 -> NULL(未绑定), 否则转义加引号
inline std::string opt(MYSQL* c, const std::string& s) {
  return s.empty() ? std::string("NULL") : q(c, s);
}

// ---- 事务小工具(S0 单聊双写 / 好友双向一致等需要多语句原子) ----
inline bool txBegin(MYSQL* c) {
  static const std::string sql = "START TRANSACTION";
  return c && mysql_real_query(c, sql.data(), (unsigned long)sql.size()) == 0;
}
inline bool txCommit(MYSQL* c) {
  static const std::string sql = "COMMIT";
  return c && mysql_real_query(c, sql.data(), (unsigned long)sql.size()) == 0;
}
inline void txRollback(MYSQL* c) {
  if (!c) return;
  static const std::string sql = "ROLLBACK";
  mysql_real_query(c, sql.data(), (unsigned long)sql.size());
}

// 事务 RAII: 出作用域未 commit 自动回滚, 防连接带未提交事务还池
struct Txn {
  MYSQL* c = nullptr;
  bool active = false;
  explicit Txn(MYSQL* conn) : c(conn) { if (c) active = txBegin(c); }
  bool ok() const { return active; }
  void commit() {
    if (active && c) {
      txCommit(c);
      active = false;
    }
  }
  ~Txn() { if (active && c) txRollback(c); }
};

} // namespace chatservice::dao
