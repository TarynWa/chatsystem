#pragma once
#include <cstdint>
#include <string>

namespace chatservice::service {

struct NewUser {
  std::string username;
  std::string password_hash;  // 已由上层(security)算好, 服务层不碰明文
  std::string nickname;
  std::string email;
  std::string phone;
};

// 落库新用户。返回 0/1101/1002; newId 出参。
int createUser(const NewUser& u, uint64_t* newId);

} // namespace chatservice::service
