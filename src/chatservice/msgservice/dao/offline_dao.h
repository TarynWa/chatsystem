// 离线消息表 offline_messages(接收方收件箱) 读写。
// S0 拉取模型: send 双写时把消息也塞这里, 接收方 PullNewMsg 从这里"认领"。
#pragma once
#include <mysql/mysql.h>
#include <string>
#include <vector>

#include "msgservice/model/message.h"

namespace chatservice::dao {

// 入收件箱(is_group=0, group_id NULL, status 默认 0 未拉取)。0 / 1002
int insertOffline(MYSQL* c, uint64_t msg_id, uint64_t user_id, uint64_t from_user,
                  int msg_type, const std::string& content,
                  const std::string& media_urls);

// 原子"认领+置已拉取+读回"(内部开事务, FOR UPDATE SKIP LOCKED 防并发双发;
// 提交后才返回, 失败回滚)。返回 0 成功(可空) / 1002。
int claimOffline(MYSQL* c, uint64_t user_id, int limit,
                 std::vector<chatservice::model::MessageRow>* out);

} // namespace chatservice::dao
