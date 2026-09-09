#include "socialservice/dao/friend_dao.h"

#include <mysql/mysql.h>

#include <cstdio>
#include <cstdlib>

#include "common/errcode.h"
#include "base/sqlutil.h"

namespace chatservice::dao {

namespace {
int execSql(MYSQL* c, const std::string& sql) {
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[friend_dao] sql error(%u): %s\n  sql=%s\n", mysql_errno(c),
            mysql_error(c), sql.c_str());
    return ErrCode::ERR_BUSY;
  }
  return ErrCode::OK;
}

// 解析一行好友关系; 调用方保证 row 非空。
// 列序: user_id, friend_id, status, remark, apply_msg, confirm_at, created_at
void readFriendRow(MYSQL_ROW row, chatservice::model::FriendRow* r) {
  r->user_id = row[0] ? strtoull(row[0], nullptr, 10) : 0;
  r->friend_id = row[1] ? strtoull(row[1], nullptr, 10) : 0;
  r->status = row[2] ? atoi(row[2]) : 0;
  if (row[3]) r->remark = row[3];
  if (row[4]) r->apply_msg = row[4];
  if (row[5]) r->confirm_at = row[5];
  if (row[6]) r->created_at = row[6];
}
} // namespace

bool isFriend(MYSQL* c, uint64_t a, uint64_t b) {
  if (!c || a == 0 || b == 0) return false;
  // 防单向脏数据: 两方向任一 status=1 即好友; 正常接受事务会建双向两行
  std::string sql = "SELECT 1 FROM friends WHERE ((user_id=" + std::to_string(a) +
                    " AND friend_id=" + std::to_string(b) + ") OR (user_id=" +
                    std::to_string(b) + " AND friend_id=" + std::to_string(a) +
                    ")) AND status=1 LIMIT 1";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) return false;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  bool found = mysql_fetch_row(res) != nullptr;
  mysql_free_result(res);
  return found;
}

int upsertFriend(MYSQL* c, uint64_t user_id, uint64_t friend_id,
                 const std::string& apply_msg) {
  if (!c) return ErrCode::ERR_BUSY;
  // 已存在行(status 3 重加)会被置回 1 并刷新 confirm_at; remark/apply_msg 不动现有值
  std::string sql =
      "INSERT INTO friends (user_id, friend_id, status, apply_msg, confirm_at) VALUES (" +
      std::to_string(user_id) + "," + std::to_string(friend_id) +
      ",1," + q(c, apply_msg) +
      ",NOW()) ON DUPLICATE KEY UPDATE status=1, confirm_at=NOW()";
  return execSql(c, sql);
}

int listFriends(MYSQL* c, uint64_t uid,
                std::vector<chatservice::model::FriendRow>* out) {
  out->clear();
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "SELECT user_id, friend_id, status, remark, apply_msg, confirm_at, created_at "
      "FROM friends WHERE user_id=" + std::to_string(uid) +
      " AND status=1 ORDER BY confirm_at DESC, id DESC";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[friend_dao] listFriends error(%u): %s\n", mysql_errno(c),
            mysql_error(c));
    return ErrCode::ERR_BUSY;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    chatservice::model::FriendRow r;
    readFriendRow(row, &r);
    out->push_back(r);
  }
  mysql_free_result(res);
  return ErrCode::OK;
}

int softDeleteFriend(MYSQL* c, uint64_t uid, uint64_t friend_id) {
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "UPDATE friends SET status=3 WHERE user_id=" + std::to_string(uid) +
      " AND friend_id=" + std::to_string(friend_id) + " AND status=1";
  return execSql(c, sql);
}

} // namespace chatservice::dao
