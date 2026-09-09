#include "msgservice/dao/offline_dao.h"

#include <cstdio>
#include <cstdlib>

#include "base/sqlutil.h"
#include "common/errcode.h"

namespace chatservice::dao {
namespace {

int execSql(MYSQL* c, const std::string& sql) {
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[offline_dao] sql error(%u): %s\n  sql=%s\n", mysql_errno(c),
            mysql_error(c), sql.c_str());
    return ErrCode::ERR_BUSY;
  }
  return ErrCode::OK;
}

} // namespace

int insertOffline(MYSQL* c, uint64_t msg_id, uint64_t user_id, uint64_t from_user,
                  int msg_type, const std::string& content,
                  const std::string& media_urls) {
  std::string sql =
      "INSERT INTO offline_messages (msg_id, user_id, from_user, msg_type, content, "
      "media_urls, is_group, group_id) VALUES (" +
      std::to_string(msg_id) + "," + std::to_string(user_id) + "," +
      std::to_string(from_user) + "," + std::to_string(msg_type) + "," +
      opt(c, content) + "," + opt(c, media_urls) + ",0,NULL)";
  return execSql(c, sql);
}

int claimOffline(MYSQL* c, uint64_t user_id, int limit,
                 std::vector<chatservice::model::MessageRow>* out) {
  out->clear();
  if (!c) return ErrCode::ERR_BUSY;
  Txn txn(c);
  if (!txn.ok()) return ErrCode::ERR_BUSY;

  // 1) 锁住本批候选(未拉取), SKIP LOCKED: 并发拉取各自锁不同行, 不互相阻塞
  std::string sql = "SELECT id FROM offline_messages WHERE user_id=" +
                    std::to_string(user_id) +
                    " AND status=0 AND is_group=0 ORDER BY id ASC LIMIT " +
                    std::to_string(limit) + " FOR UPDATE SKIP LOCKED";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[offline_dao] claim select error(%u): %s\n", mysql_errno(c),
            mysql_error(c));
    return ErrCode::ERR_BUSY;  // Txn 析构回滚
  }
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  std::vector<uint64_t> ids;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr)
    if (row[0]) ids.push_back(strtoull(row[0], nullptr, 10));
  mysql_free_result(res);
  if (ids.empty()) {
    txn.commit();
    return ErrCode::OK;  // 空收件箱
  }

  // 2) 置为已拉取
  std::string in;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) in += ",";
    in += std::to_string(ids[i]);
  }
  std::string upd = "UPDATE offline_messages SET status=1, delivered_at=NOW() WHERE id IN (" +
                    in + ")";
  if (execSql(c, upd) != ErrCode::OK) return ErrCode::ERR_BUSY;

  // 3) 读回完整内容
  std::string sel =
      "SELECT msg_id, from_user, msg_type, content, media_urls, created_at "
      "FROM offline_messages WHERE id IN (" + in + ") ORDER BY id ASC";
  if (mysql_real_query(c, sel.data(), (unsigned long)sel.size()) != 0) {
    fprintf(stderr, "[offline_dao] claim read error(%u): %s\n", mysql_errno(c),
            mysql_error(c));
    return ErrCode::ERR_BUSY;
  }
  res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    chatservice::model::MessageRow m;
    m.msg_id = row[0] ? strtoull(row[0], nullptr, 10) : 0;
    m.from_user = row[1] ? strtoull(row[1], nullptr, 10) : 0;
    m.to_user = user_id;
    m.msg_type = row[2] ? atoi(row[2]) : 0;
    if (row[3]) m.content = row[3];
    if (row[4]) m.media_urls = row[4];
    if (row[5]) m.created_at = row[5];
    out->push_back(m);
  }
  mysql_free_result(res);

  txn.commit();
  return ErrCode::OK;
}

} // namespace chatservice::dao
