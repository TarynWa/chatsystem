#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>

namespace chatservice::dao {

// sessions 表一行(仅会话吊销/登出所需字段)
struct SessionRow {
  uint64_t id = 0;
  uint64_t user_id = 0;
  std::string device_id;
  std::string token;        // 当前 access token
  std::string refresh_token;
  std::string ip;
  std::string user_agent;
};

// 该 (user, device) 当前是否已有有效会话(同设备重复登录=覆盖，据此判断是否"新增设备")
bool hasActiveSession(MYSQL* c, uint64_t user_id, const std::string& device_id);

// 该用户当前有效会话里，除本设备外的设备数
int countActiveOthers(MYSQL* c, uint64_t user_id, const std::string& device_id);

// 取该用户最久未活跃的其它设备会话(kick_oldest 用)
bool pickOldestActiveOthers(MYSQL* c, uint64_t user_id,
                            const std::string& device_id, SessionRow* out);

// 覆盖登录: 注销该设备此前全部有效会话
void deactivateByDevice(MYSQL* c, uint64_t user_id, const std::string& device_id);

// 按 access 或 refresh token 找有效会话
bool findActiveByToken(MYSQL* c, const std::string& token, SessionRow* out);

// 插入一条会话: access 存 token 列, refresh 存 refresh_token 列, 有效期以秒计
bool insertSession(MYSQL* c, uint64_t user_id, const std::string& device_id,
                   const std::string& access, const std::string& refresh,
                   long accessSec, long refreshSec, const std::string& ip,
                   const std::string& user_agent);

// 置会话失效(登出/kick)
void markRevoked(MYSQL* c, uint64_t id);

} // namespace chatservice::dao
