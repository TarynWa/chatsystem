// 单聊消息表 messages_YYYYMM(按月分表) 的读写。S0 只落当月表。
#pragma once
#include <mysql/mysql.h>
#include <string>
#include <vector>

#include "msgservice/model/message.h"

namespace chatservice::dao {

// 按本地时区拼当月表名 "messages_YYYYMM"
std::string currentMonthTable();

// 插一条单聊消息; content/media_urls 空串 -> NULL。0 成功 / ErrCode(1002 DB,表未建同归 1002)
int insertMessage(MYSQL* c, const std::string& table, uint64_t msg_id,
                  uint64_t from_user, uint64_t to_user, int msg_type,
                  const std::string& content, const std::string& media_urls);

// 回读 created_at(发送双写事务内用, 与 insert 同连接)
int queryCreatedAt(MYSQL* c, const std::string& table, uint64_t msg_id,
                   std::string* created_at);

// 双向会话历史, msg_id 倒序游标分页; limit 内传入要取的条数, 内部多取 1 判 has_more
// 返回 0 成功(可能空), 失败 1002。
int fetchHistory(MYSQL* c, const std::string& table, uint64_t me, uint64_t peer,
                 uint64_t before_msg_id, int limit,
                 std::vector<chatservice::model::MessageRow>* out, bool* has_more);

} // namespace chatservice::dao
