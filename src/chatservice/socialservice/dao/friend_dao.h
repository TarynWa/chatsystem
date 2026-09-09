// 好友关系表 friends 读写(S0: 只在"接受"事务里建 status=1 双行; 删除软删 status=3)。
#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>
#include <vector>

#include "socialservice/model/social.h"

namespace chatservice::dao {

// a、b 是否为好友(两方向任一 status=1 即算; 防历史脏数据单向)。true=已是好友。
bool isFriend(MYSQL* c, uint64_t a, uint64_t b);

// 写/激活一条好友关系(user_id, friend_id)。INSERT status=1 + confirm_at=NOW();
// 已存在(含 status=3 软删后重加)则 ON DUPLICATE 置回 status=1。remark/apply_msg 保留现有。
int upsertFriend(MYSQL* c, uint64_t user_id, uint64_t friend_id,
                 const std::string& apply_msg);

// 列出 uid 的全部好友(status=1), 按成为好友时间倒序。
int listFriends(MYSQL* c, uint64_t uid,
                std::vector<chatservice::model::FriendRow>* out);

// 软删一条(user_id, friend_id): status=1 -> 3。幂等(不存在/已删也算成功)。
int softDeleteFriend(MYSQL* c, uint64_t uid, uint64_t friend_id);

} // namespace chatservice::dao
