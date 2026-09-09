// 社交域纯结构体契约(dao/service/impl 层间用, 不引 protobuf)
#pragma once
#include <cstdint>
#include <string>

namespace chatservice::model {

// 好友关系行(friends)。S0 只落 status=1(接受)/3(软删)。
struct FriendRow {
  uint64_t user_id = 0;
  uint64_t friend_id = 0;
  int status = 0;      // 0待/1已确认/2拒/3删(状态机 0/2 与 TCC 场景留后续)
  std::string remark;  // 我侧备注(S0 恒空)
  std::string apply_msg;
  std::string confirm_at;  // 成为好友时间 "YYYY-MM-DD HH:MM:SS"(未确认为空)
  std::string created_at;
};

// 好友申请行(friend_requests)
struct RequestRow {
  uint64_t id = 0;
  uint64_t from_user = 0;
  uint64_t to_user = 0;
  int status = 0;  // 0待处理/1已接受/2已拒绝/3已过期
  std::string apply_msg;
  std::string expire_at;   // "YYYY-MM-DD HH:MM:SS"(申请+3 天)
  std::string handled_at;  // 未处理为空
};

// 黑名单行(blacklist)
struct BlackRow {
  uint64_t blocked_user_id = 0;
  std::string reason;
  std::string created_at;
};

} // namespace chatservice::model
