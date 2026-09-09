// 消息业务流程编排层: 不碰 SQL 字符串, 不依赖 protobuf(见 README §层职责红线)。
// 线程安全: 方法可被 4 个 nwl IO 线程并发调用; 无共享可变状态(雪花/连接池自线程安全)。
#pragma once
#include <string>
#include <vector>

#include "msgservice/model/message.h"

namespace chatservice::service {

// 发送单聊: 雪花 id → 单事务双写 messages_{当月} + offline_messages → 回读 created_at。
// 返回 0 成功; 失败 ErrCode(1001/1401 由调用方先判, 这里二次防线 + 1002)。
int sendMessage(uint64_t from_user, uint64_t to_user, int msg_type,
                const std::string& content, const std::string& media_urls,
                chatservice::model::SentMessage* out);

// 拉取新消息(认领 offline 收件箱)。limit<=0 -> 50; 封顶 100。0 成功(可能空)。
int pullMessages(uint64_t user_id, int limit,
                 std::vector<chatservice::model::MessageRow>* out);

// 双向会话历史(msg_id 倒序游标分页)。limit<=0 -> 20; 封顶 50。
int getHistory(uint64_t me, uint64_t peer, uint64_t before_msg_id, int limit,
               std::vector<chatservice::model::MessageRow>* out, bool* has_more);

} // namespace chatservice::service
