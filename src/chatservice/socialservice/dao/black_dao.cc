#include "socialservice/dao/black_dao.h"

#include <cstdio>
#include <cstdlib>

#include "common/errcode.h"
#include "base/sqlutil.h"

namespace chatservice::dao {

namespace {
int execSql(MYSQL* c, const std::string& sql) {
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[black_dao] sql error(%u): %s\n  sql=%s\n", mysql_errno(c),
            mysql_error(c), sql.c_str());
    return ErrCode::ERR_BUSY;
  }
  return ErrCode::OK;
}
} // namespace

int addBlack(MYSQL* c, uint64_t user_id, uint64_t blocked_user_id,
             const std::string& reason) {
  if (!c) return ErrCode::ERR_BUSY;
  // 幂等 upsert: 已拉黑则只刷新 reason
  std::string sql =
      "INSERT INTO blacklist (user_id, blocked_user_id, reason) VALUES (" +
      std::to_string(user_id) + "," + std::to_string(blocked_user_id) + "," +
      q(c, reason) +
      ") ON DUPLICATE KEY UPDATE reason=VALUES(reason)";
  return execSql(c, sql);
}

int removeBlack(MYSQL* c, uint64_t user_id, uint64_t blocked_user_id) {
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "DELETE FROM blacklist WHERE user_id=" + std::to_string(user_id) +
      " AND blocked_user_id=" + std::to_string(blocked_user_id);
  return execSql(c, sql);
}

int listBlack(MYSQL* c, uint64_t uid,
              std::vector<chatservice::model::BlackRow>* out) {
  out->clear();
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "SELECT blocked_user_id, reason, created_at FROM blacklist WHERE user_id=" +
      std::to_string(uid) + " ORDER BY created_at DESC, id DESC";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[black_dao] listBlack error(%u): %s\n", mysql_errno(c),
            mysql_error(c));
    return ErrCode::ERR_BUSY;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    chatservice::model::BlackRow b;
    b.blocked_user_id = row[0] ? strtoull(row[0], nullptr, 10) : 0;
    if (row[1]) b.reason = row[1];
    if (row[2]) b.created_at = row[2];
    out->push_back(b);
  }
  mysql_free_result(res);
  return ErrCode::OK;
}

bool isBlocked(MYSQL* c, uint64_t a, uint64_t b) {
  if (!c || a == 0 || b == 0) return false;
  std::string sql =
      "SELECT 1 FROM blacklist WHERE user_id=" + std::to_string(a) +
      " AND blocked_user_id=" + std::to_string(b) + " LIMIT 1";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) return false;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  bool found = mysql_fetch_row(res) != nullptr;
  mysql_free_result(res);
  return found;
}

} // namespace chatservice::dao
