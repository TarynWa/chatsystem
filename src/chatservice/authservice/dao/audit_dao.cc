#include "authservice/dao/audit_dao.h"

#include "authservice/dao/db_pool.h"
#include "authservice/dao/sqlutil.h"

namespace chatservice::dao {

void addAudit(uint64_t user_id, const std::string& username,
              const std::string& action, const std::string& resource_type,
              const std::string& resource_id, const std::string& detail,
              const std::string& ip, const std::string& user_agent,
              bool success, const std::string& error_msg) {
  DbConn conn;
  if (!conn.ok()) return;
  // detail 是 JSON 列; 空串 -> NULL。resource_id/type 列宽有限, 截断防超限。
  std::string rid = resource_id.substr(0, 64);
  std::string rty = resource_type.substr(0, 64);
  std::string sql =
      "INSERT INTO audit_logs "
      "(user_id,username,action,resource_type,resource_id,detail,ip,user_agent,"
      "status,error_msg) VALUES (" +
      (user_id ? std::to_string(user_id) : std::string("NULL")) + "," +
      q(conn.c, username) + "," + q(conn.c, action) + "," + q(conn.c, rty) +
      "," + q(conn.c, rid) + "," + opt(conn.c, detail) + "," +
      q(conn.c, ip) + "," + q(conn.c, user_agent) + "," +
      std::to_string(success ? 1 : 0) + "," + q(conn.c, error_msg) + ")";
  mysql_real_query(conn.c, sql.data(), sql.size());
}

} // namespace chatservice::dao
