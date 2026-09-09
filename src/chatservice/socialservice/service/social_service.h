// 社交业务流程编排层: 不碰 SQL 字符串, 不依赖 protobuf(见 README §层职责红线)。
// 线程安全: 方法可被 4 个 nwl IO 线程并发调用; 无共享可变状态(连接池自线程安全)。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "socialservice/model/social.h"

namespace chatservice::service {

// 发起好友申请 from→to。前置序: 非自己(1506)/已是好友(1501)/已有 pending(1502)/
//   对方拉黑你(1507)/你拉黑对方(1508); 通过则落一条 status=0 的申请(3 天过期),
//   回填 request_id 与 expire_at。返回 0 成功, 否则 ErrCode。
int addFriend(uint64_t from_user, uint64_t to_user,
              const std::string& apply_msg, uint64_t* request_id,
              std::string* expire_at);

// 处理好友申请(handler=被申请人)。action: 1 接受 / 2 拒绝。
//   接受: 同一事务内先护栏置 status=1, 再双向建/激活 friends status=1。
//   返回 0 / 1503(不存在或已处理) / 1504(已过期) / 1505(非处理人) / 1002。
int handleFriendRequest(uint64_t handler_id, uint64_t request_id, int action);

// 删除好友: 两侧 friends 软删 status=3。幂等(不存在也算成功)。
int deleteFriend(uint64_t user_id, uint64_t friend_id);

// 好友列表(status=1, 双向已确认)。0 成功(可能空)。
int listFriends(uint64_t user_id, std::vector<chatservice::model::FriendRow>* out);

// 加黑名单(幂等)/移除黑名单(幂等)/黑名单列表。
int addBlacklist(uint64_t user_id, uint64_t blocked_user_id,
                 const std::string& reason);
int removeBlacklist(uint64_t user_id, uint64_t blocked_user_id);
int listBlacklist(uint64_t user_id,
                  std::vector<chatservice::model::BlackRow>* out);

} // namespace chatservice::service
