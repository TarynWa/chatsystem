// 好友申请表 friend_requests 读写。应用层去重 pending; expire_at 需显式给(表无默认)。
#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>

#include "socialservice/model/social.h"

namespace chatservice::dao {

// from→to 是否已有未处理申请(status=0 且未过期)。true=有(应用层去重, 防重复申请)。
int hasPendingRequest(MYSQL* c, uint64_t from_user, uint64_t to_user, bool* out);

// 插一条申请(expire_at=NOW()+INTERVAL 3 DAY 显式), 回填新申请 id 与 expire_at 文案。
int insertRequest(MYSQL* c, uint64_t from_user, uint64_t to_user,
                  const std::string& apply_msg, uint64_t* new_id,
                  std::string* expire_at);

// 按 id 读一条申请; *found=false 表示不存在。
int loadRequest(MYSQL* c, uint64_t id, chatservice::model::RequestRow* out,
                bool* found);

// 处理护栏: UPDATE ... WHERE id=? AND status=0 AND to_user=? AND expire_at>NOW()。
// 受影响行数回填 *affected(0 = 已被并发处理或过期, 调用方再决定 1503/1504)。
int acceptRequest(MYSQL* c, uint64_t id, uint64_t handler_id, int* affected);
int rejectRequest(MYSQL* c, uint64_t id, uint64_t handler_id, int* affected);

// 申请是否已过期(status=0 且 expire_at<=NOW())。
bool requestExpired(MYSQL* c, uint64_t id);

// 把过期未处理申请标记为 status=3(幂等)。
int markRequestExpired(MYSQL* c, uint64_t id);

} // namespace chatservice::dao
