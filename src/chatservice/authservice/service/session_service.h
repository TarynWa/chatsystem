#pragma once
#include <cstdint>
#include <string>

#include "authservice/model/user.h"

namespace chatservice::service {

// 运行参数(主启动时从 conf 装载后注入; 便于测试)
struct Policy {
  long accessSec = 7200;                 // access 2h
  long refreshSec = 7L * 24 * 3600;      // refresh 7d
  int maxDevices = 5;                    // 多端上限
  bool kickOldest = true;                // true: 挤最久未活跃; false: 拒新(reject_new)
  int maxFail = 5;                       // 防爆破阈值
  int lockMinutes = 30;                  // 锁定分钟
};

// 完整登录: 锁检查 → 找账号 → 验密(失败计数) → 会话签发 + audit。
// user/tokens 仅成功时回填。返回 ErrCode。
int login(const std::string& account, const std::string& password,
          const model::DeviceInfo& dev, const std::string& ip,
          const std::string& ua, const Policy& pol, model::UserRow* user,
          model::SessionPair* tokens);

// 注册成功自动登录: 直接按 uid 签发(不再走密码), 与 login 共用签发逻辑 + audit。
int autoLogin(uint64_t user_id, const std::string& username,
              const model::DeviceInfo& dev, const std::string& ip,
              const std::string& ua, const Policy& pol,
              model::SessionPair* tokens);

// Redis 反查 token 归属 + 滑动续期。0 有效 / 1201 无效。
int verifyToken(const std::string& token, const Policy& pol,
                uint64_t* user_id, std::string* device_id);

// 登出(幂等): 吊销 MySQL + Redis 会话。带出 uid/did(0 表示未知 token)。
int logout(const std::string& token, uint64_t* user_id,
           std::string* device_id);

} // namespace chatservice::service
