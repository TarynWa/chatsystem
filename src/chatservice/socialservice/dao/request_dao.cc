#include "socialservice/dao/request_dao.h"

#include <cstdio>
#include <cstdlib>

#include "common/errcode.h"
#include "base/sqlutil.h"

namespace chatservice::dao {

namespace {
int execSql(MYSQL* c, const std::string& sql) {
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) {
    fprintf(stderr, "[request_dao] sql error(%u): %s\n  sql=%s\n", mysql_errno(c),
            mysql_error(c), sql.c_str());
    return ErrCode::ERR_BUSY;
  }
  return ErrCode::OK;
}

// 列序: id, from_user, to_user, status, apply_msg, expire_at, handled_at
void readRequestRow(MYSQL_ROW row, chatservice::model::RequestRow* r) {
  r->id = row[0] ? strtoull(row[0], nullptr, 10) : 0;
  r->from_user = row[1] ? strtoull(row[1], nullptr, 10) : 0;
  r->to_user = row[2] ? strtoull(row[2], nullptr, 10) : 0;
  r->status = row[3] ? atoi(row[3]) : 0;
  if (row[4]) r->apply_msg = row[4];
  if (row[5]) r->expire_at = row[5];
  if (row[6]) r->handled_at = row[6];
}
} // namespace

int hasPendingRequest(MYSQL* c, uint64_t from_user, uint64_t to_user, bool* out) {
  *out = false;
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "SELECT id FROM friend_requests WHERE from_user=" +
      std::to_string(from_user) + " AND to_user=" + std::to_string(to_user) +
      " AND status=0 AND expire_at>NOW() LIMIT 1";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) return ErrCode::ERR_BUSY;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  *out = mysql_fetch_row(res) != nullptr;
  mysql_free_result(res);
  return ErrCode::OK;
}

int insertRequest(MYSQL* c, uint64_t from_user, uint64_t to_user,
                  const std::string& apply_msg, uint64_t* new_id,
                  std::string* expire_at) {
  if (!c) return ErrCode::ERR_BUSY;
  // expire_at 列 NOT NULL 且无默认值: 必须显式给 NOW()+INTERVAL 3 DAY, 否则 MySQL 1364
  std::string sql =
      "INSERT INTO friend_requests (from_user, to_user, apply_msg, expire_at) VALUES (" +
      std::to_string(from_user) + "," + std::to_string(to_user) + "," +
      q(c, apply_msg) + ", NOW()+INTERVAL 3 DAY)";
  int rc = execSql(c, sql);
  if (rc != ErrCode::OK) return rc;
  if (new_id) *new_id = (uint64_t)mysql_insert_id(c);
  if (expire_at) {
    std::string qs = "SELECT expire_at FROM friend_requests WHERE id=" +
                     std::to_string(*new_id) + " LIMIT 1";
    if (mysql_real_query(c, qs.data(), (unsigned long)qs.size()) == 0) {
      MYSQL_RES* res = mysql_store_result(c);
      if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) expire_at->assign(row[0]);
        mysql_free_result(res);
      }
    }
  }
  return ErrCode::OK;
}

int loadRequest(MYSQL* c, uint64_t id, chatservice::model::RequestRow* out,
                bool* found) {
  *found = false;
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "SELECT id, from_user, to_user, status, apply_msg, expire_at, handled_at "
      "FROM friend_requests WHERE id=" + std::to_string(id) + " LIMIT 1";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) return ErrCode::ERR_BUSY;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return ErrCode::ERR_BUSY;
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row) {
    readRequestRow(row, out);
    *found = true;
  }
  mysql_free_result(res);
  return ErrCode::OK;
}

namespace {
// 处理语句公共部分: status 由调用方给(1 接受 / 2 拒绝)
int handleById(MYSQL* c, uint64_t id, uint64_t handler_id, int target_status,
               int* affected) {
  *affected = 0;
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "UPDATE friend_requests SET status=" + std::to_string(target_status) +
      ", handled_at=NOW(), handler_id=" + std::to_string(handler_id) +
      " WHERE id=" + std::to_string(id) + " AND status=0 AND to_user=" +
      std::to_string(handler_id) + " AND expire_at>NOW()";
  int rc = execSql(c, sql);
  if (rc != ErrCode::OK) return rc;
  *affected = (int)mysql_affected_rows(c);
  return ErrCode::OK;
}
} // namespace

int acceptRequest(MYSQL* c, uint64_t id, uint64_t handler_id, int* affected) {
  return handleById(c, id, handler_id, 1, affected);
}

int rejectRequest(MYSQL* c, uint64_t id, uint64_t handler_id, int* affected) {
  return handleById(c, id, handler_id, 2, affected);
}

bool requestExpired(MYSQL* c, uint64_t id) {
  if (!c) return false;
  std::string sql = "SELECT 1 FROM friend_requests WHERE id=" +
                    std::to_string(id) +
                    " AND status=0 AND expire_at<=NOW() LIMIT 1";
  if (mysql_real_query(c, sql.data(), (unsigned long)sql.size()) != 0) return false;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return false;
  bool expired = mysql_fetch_row(res) != nullptr;
  mysql_free_result(res);
  return expired;
}

int markRequestExpired(MYSQL* c, uint64_t id) {
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql = "UPDATE friend_requests SET status=3, handled_at=NOW() WHERE id=" +
                    std::to_string(id) + " AND status=0";
  return execSql(c, sql);
}

} // namespace chatservice::dao
