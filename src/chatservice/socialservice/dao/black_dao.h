// 黑名单表 blacklist(表名单数, 注意不是 blacklists)读写。
#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>
#include <vector>

#include "socialservice/model/social.h"

namespace chatservice::dao {

// 加黑名单(幂等): 已存在则刷新 reason。返回 ErrCode。
int addBlack(MYSQL* c, uint64_t user_id, uint64_t blocked_user_id,
             const std::string& reason);

// 移除黑名单(幂等: 不存在也算成功)。
int removeBlack(MYSQL* c, uint64_t user_id, uint64_t blocked_user_id);

// 列出 uid 的全部黑名单, 按拉黑时间倒序。
int listBlack(MYSQL* c, uint64_t uid,
              std::vector<chatservice::model::BlackRow>* out);

// a 是否已拉黑 b(user_id=a, blocked_user_id=b)。
bool isBlocked(MYSQL* c, uint64_t a, uint64_t b);

} // namespace chatservice::dao
