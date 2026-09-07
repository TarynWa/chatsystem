#include "authservice/dao/session_dao.h"

#include <cstring>

#include "authservice/dao/sqlutil.h"

namespace chatservice::dao {

namespace {
const char* kSelectCols =
    "SELECT id,user_id,device_id,token,refresh_token,ip,user_agent FROM sessions ";

SessionRow readSessionRow(MYSQL_RES* res) {
  SessionRow r;
  MYSQL_ROW row = mysql_fetch_row(res);
  if (!row) return r;
  // 列序: id,user_id,device_id,token,refresh_token,ip,user_agent
  if (row[0]) r.id = strtoull(row[0], nullptr, 10);
  if (row[1]) r.user_id = strtoull(row[1], nullptr, 10);
  if (row[2]) r.device_id = row[2];
  if (row[3]) r.token = row[3];
  if (row[4]) r.refresh_token = row[4];
  if (row[5]) r.ip = row[5];
  if (row[6]) r.user_agent = row[6];
  return r;
}

SessionRow queryOne(MYSQL* c, const std::string& sql) {
  SessionRow r;
  if (!c) return r;
  if (mysql_real_query(c, sql.data(), sql.size()) != 0) return r;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return r;
  r = readSessionRow(res);
  mysql_free_result(res);
  return r;
}
} // namespace

bool hasActiveSession(MYSQL* c, uint64_t user_id, const std::string& device_id) {
  SessionRow r = queryOne(
      c, std::string("SELECT id,user_id,device_id,token,refresh_token,ip,user_agent "
                     "FROM sessions WHERE user_id=") +
             std::to_string(user_id) + " AND device_id=" + q(c, device_id) +
             " AND status=1 LIMIT 1");
  return r.id != 0;
}

int countActiveOthers(MYSQL* c, uint64_t user_id, const std::string& device_id) {
  if (!c) return 0;
  std::string sql =
      "SELECT COUNT(DISTINCT device_id) FROM sessions WHERE user_id=" +
      std::to_string(user_id) + " AND status=1 AND device_id<>" +
      q(c, device_id);
  if (mysql_real_query(c, sql.data(), sql.size()) != 0) return 0;
  MYSQL_RES* res = mysql_store_result(c);
  if (!res) return 0;
  MYSQL_ROW row = mysql_fetch_row(res);
  int n = (row && row[0]) ? atoi(row[0]) : 0;
  mysql_free_result(res);
  return n;
}

bool pickOldestActiveOthers(MYSQL* c, uint64_t user_id,
                            const std::string& device_id, SessionRow* out) {
  std::string sql = std::string(kSelectCols) + "WHERE user_id=" +
                    std::to_string(user_id) + " AND status=1 AND device_id<>" +
                    q(c, device_id) + " ORDER BY created_at ASC LIMIT 1";
  SessionRow r = queryOne(c, sql);
  if (out) *out = r;
  return r.id != 0;
}

void deactivateByDevice(MYSQL* c, uint64_t user_id, const std::string& device_id) {
  if (!c) return;
  std::string sql = "UPDATE sessions SET status=0,updated_at=NOW() WHERE user_id=" +
                    std::to_string(user_id) + " AND device_id=" + q(c, device_id) +
                    " AND status=1";
  mysql_real_query(c, sql.data(), sql.size());
}

bool findActiveByToken(MYSQL* c, const std::string& token, SessionRow* out) {
  std::string sql = std::string(kSelectCols) +
                    "WHERE status=1 AND (token=" + q(c, token) +
                    " OR refresh_token=" + q(c, token) + ") LIMIT 1";
  SessionRow r = queryOne(c, sql);
  if (out) *out = r;
  return r.id != 0;
}

bool insertSession(MYSQL* c, uint64_t user_id, const std::string& device_id,
                   const std::string& access, const std::string& refresh,
                   long accessSec, long refreshSec, const std::string& ip,
                   const std::string& user_agent) {
  if (!c) return false;
  std::string sql =
      "INSERT INTO sessions "
      "(token,user_id,device_id,refresh_token,refresh_expires_at,expires_at,"
      "status,ip,user_agent) VALUES (" +
      q(c, access) + "," + std::to_string(user_id) + "," + q(c, device_id) +
      "," + q(c, refresh) + ",DATE_ADD(NOW(), INTERVAL " +
      std::to_string(refreshSec) + " SECOND),DATE_ADD(NOW(), INTERVAL " +
      std::to_string(accessSec) +
      " SECOND),1," + q(c, ip) + "," + q(c, user_agent) + ")";
  return mysql_real_query(c, sql.data(), sql.size()) == 0;
}

void markRevoked(MYSQL* c, uint64_t id) {
  if (!c) return;
  std::string sql = "UPDATE sessions SET status=0,updated_at=NOW() WHERE id=" +
                    std::to_string(id);
  mysql_real_query(c, sql.data(), sql.size());
}

} // namespace chatservice::dao
