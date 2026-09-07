// 用户/会话领域纯结构体（dao/service/rpc 层间契约，不依赖 protobuf）
#pragma once
#include <cstdint>
#include <string>

namespace chatservice::model {

struct UserRow {
  bool   found = false;   // 查询是否命中
  uint64_t id = 0;
  std::string username;
  std::string password_hash;  // 只读不回传客户端
  std::string nickname;
  std::string email;
  std::string phone;
  int      status = 0;
  bool     deleted = false;   // deleted_at 非空
};

struct DeviceInfo {
  std::string device_id;
  std::string device_name;
  int      device_type = 0;
  std::string os_version;
  std::string app_version;
};

// 签发的会话
struct SessionPair {
  std::string access_token;
  int      access_expires_in = 0;   // 秒
  std::string refresh_token;
  int      refresh_expires_in = 0;  // 秒
};

} // namespace chatservice::model
