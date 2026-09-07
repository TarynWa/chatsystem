// SQL 小工具：转义 + 拼串（S0 用字符串拼 SQL + 转义防注入；
// README §5.5 目标为 mysql_stmt_* 预处理语句，M4 统一替换）
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

} // namespace chatservice::dao
