#include "authservice/dao/user_dao.h"

#include <cstring>

#include "common/errcode.h"
#include "authservice/dao/sqlutil.h"

namespace chatservice::dao {

namespace {
model::UserRow readUserRow(MYSQL_RES* res) {
  model::UserRow r;
  MYSQL_ROW row = mysql_fetch_row(res);
  if (!row) return r;
  // 列序: id, username, password, nickname, email, phone, status, is_deleted
  r.found = true;
  if (row[0]) r.id = strtoull(row[0], nullptr, 10);
  if (row[1]) r.username = row[1];
  if (row[2]) r.password_hash = row[2];
  if (row[3]) r.nickname = row[3];
  if (row[4]) r.email = row[4];
  if (row[5]) r.phone = row[5];
  if (row[6]) r.status = atoi(row[6]);
  if (row[7]) r.deleted = atoi(row[7]) != 0;
  return r;
}
} // namespace

model::UserRow findUserBy(MYSQL* c, const std::string& field,
                          const std::string& value) {
  model::UserRow r;
  if (!c || field != "username" && field != "email" && field != "phone")
    return r;

  std::string sql =
      "SELECT id,username,password,nickname,email,phone,status,"
      "(deleted_at IS NOT NULL) FROM users WHERE " +
      field + "=" + q(c, value) + " LIMIT 1";
  if (mysql_real_query(c, sql.data(), sql.size()) != 0) return r;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return r;
  r = readUserRow(res);
  mysql_free_result(res);
  return r;
}

int insertUser(MYSQL* c, const std::string& username,
               const std::string& passwordHash, const std::string& nickname,
               const std::string& email, const std::string& phone,
               uint64_t* newId) {
  if (!c) return ErrCode::ERR_BUSY;
  std::string sql =
      "INSERT INTO users (username,password,nickname,email,phone) VALUES (" +
      q(c, username) + "," + q(c, passwordHash) + "," + q(c, nickname) + "," +
      opt(c, email) + "," + opt(c, phone) + ")";
  if (mysql_real_query(c, sql.data(), sql.size()) != 0) {
    if (mysql_errno(c) == 1062) return ErrCode::ERR_USER_EXIST;
    return ErrCode::ERR_BUSY;
  }
  if (newId) *newId = (uint64_t)mysql_insert_id(c);
  return ErrCode::OK;
}

void updateLoginInfo(MYSQL* c, uint64_t id, const std::string& ip,
                     const std::string& device) {
  if (!c) return;
  std::string sql =
      "UPDATE users SET last_login_at=NOW(), last_ip=" + q(c, ip) +
      ", device_info=" + q(c, device) + " WHERE id=" + std::to_string(id);
  mysql_real_query(c, sql.data(), sql.size());
}

} // namespace chatservice::dao
