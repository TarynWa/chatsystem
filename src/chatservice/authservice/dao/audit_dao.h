#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>

namespace chatservice::dao {

// 追加一条审计(register/login/logout/...)。内部自取连接, 失败不影响主流程。
// userId==0 表示未登录(如注册失败场景); detail 为空串 -> NULL
void addAudit(uint64_t user_id, const std::string& username,
              const std::string& action, const std::string& resource_type,
              const std::string& resource_id, const std::string& detail,
              const std::string& ip, const std::string& user_agent,
              bool success, const std::string& error_msg);

} // namespace chatservice::dao
