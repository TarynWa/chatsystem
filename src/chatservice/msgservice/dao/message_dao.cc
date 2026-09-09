#include "msgservice/dao/message_dao.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "base/sqlutil.h"
#include "common/errcode.h"

namespace chatservice::dao {
namespace {

// 执行非查询 SQL; 失败打日志并返回错误号(DB 层统一返回 1002)
int execSql(MYSQL* c, const std::string& sql) {
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[message_dao] sql error(%u): %s\n  sql=%s\n", mysql_errno(c),
            mysql_error(c), sql.c_str());
    return ErrCode::ERR_BUSY;
  }
  return ErrCode::OK;
}

} // namespace

std::string currentMonthTable() {
  time_t t = time(nullptr);
  struct tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  snprintf(buf, sizeof(buf), "messages_%04d%02d", tm.tm_year + 1900,
           tm.tm_mon + 1);
  return buf;
}

int insertMessage(MYSQL* c, const std::string& table, uint64_t msg_id,
                  uint64_t from_user, uint64_t to_user, int msg_type,
                  const std::string& content, const std::string& media_urls) {
  std::string sql = "INSERT INTO " + table +
                    " (msg_id, from_user, to_user, msg_type, content, media_urls) VALUES (" +
                    std::to_string(msg_id) + "," + std::to_string(from_user) + "," +
                    std::to_string(to_user) + "," + std::to_string(msg_type) + "," +
                    opt(c, content) + "," + opt(c, media_urls) + ")";
  return execSql(c, sql);
}

int queryCreatedAt(MYSQL* c, const std::string& table, uint64_t msg_id,
                   std::string* created_at) {
  std::string sql = "SELECT created_at FROM " + table +
                    " WHERE msg_id=" + std::to_string(msg_id) + " LIMIT 1";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[message_dao] queryCreatedAt error(%u): %s\n", mysql_errno(c),
            mysql_error(c));
    return ErrCode::ERR_BUSY;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row && row[0]) created_at->assign(row[0]);
  mysql_free_result(res);
  return ErrCode::OK;
}

int fetchHistory(MYSQL* c, const std::string& table, uint64_t me, uint64_t peer,
                 uint64_t before_msg_id, int limit,
                 std::vector<chatservice::model::MessageRow>* out, bool* has_more) {
  out->clear();
  if (has_more) *has_more = false;
  // 注: 方向 OR 组必须整体加括号, 否则 AND 优先级高于 OR, 前一个方向会绕过 msg_id 边界
  std::string sql =
      "SELECT msg_id, from_user, to_user, msg_type, content, media_urls, created_at FROM " +
      table + " WHERE ((from_user=" + std::to_string(me) + " AND to_user=" +
      std::to_string(peer) + ") OR (from_user=" + std::to_string(peer) +
      " AND to_user=" + std::to_string(me) + "))";
  if (before_msg_id > 0) sql += " AND msg_id<" + std::to_string(before_msg_id);
  sql += " ORDER BY msg_id DESC LIMIT " + std::to_string(limit + 1);

  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[message_dao] fetchHistory error(%u): %s\n", mysql_errno(c),
            mysql_error(c));
    return ErrCode::ERR_BUSY;
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    chatservice::model::MessageRow m;
    m.msg_id = row[0] ? strtoull(row[0], nullptr, 10) : 0;
    m.from_user = row[1] ? strtoull(row[1], nullptr, 10) : 0;
    m.to_user = row[2] ? strtoull(row[2], nullptr, 10) : 0;
    m.msg_type = row[3] ? atoi(row[3]) : 0;
    if (row[4]) m.content = row[4];
    if (row[5]) m.media_urls = row[5];
    if (row[6]) m.created_at = row[6];
    out->push_back(m);
  }
  mysql_free_result(res);
  if ((int)out->size() > limit) {
    if (has_more) *has_more = true;
    out->resize(limit);  // 多取的那行是"更早的一条", 丢弃只作 has_more 判定
  }
  return ErrCode::OK;
}

} // namespace chatservice::dao
