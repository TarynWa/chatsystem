// 防爆破失败计数(README §5.3):
//   login:fail:{account}   INCR 计数(滑动 30min 窗口)
//   login:lock:{account}   达到 max_fail 后置锁, TTL=lock_minutes
#pragma once
#include <string>

namespace chatservice::cache {

// 账号是否已锁定
bool failLocked(const std::string& account);

// 记一次失败; 返回是否刚刚触发锁定(达到阈值)
bool recordFail(const std::string& account, int maxFail, int lockMinutes);

// 登录成功清计数
void clearFail(const std::string& account);

} // namespace chatservice::cache
