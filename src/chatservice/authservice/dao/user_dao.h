#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>

#include "authservice/model/user.h"

namespace chatservice::dao {

// 按唯一字段查询用户；field 白名单 username|email|phone（杜绝拼接注入）
model::UserRow findUserBy(MYSQL* c, const std::string& field,
                          const std::string& value);

// 插入用户。返回 0 成功 / 1101 唯一键冲突 / 1002 DB 异常；newId 出参。
int insertUser(MYSQL* c, const std::string& username,
               const std::string& passwordHash, const std::string& nickname,
               const std::string& email, const std::string& phone,
               uint64_t* newId);

// 登录成功后更新 users 最近登录信息
void updateLoginInfo(MYSQL* c, uint64_t id, const std::string& ip,
                     const std::string& device);

} // namespace chatservice::dao
