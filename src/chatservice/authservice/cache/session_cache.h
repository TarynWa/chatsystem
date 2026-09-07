// Redis 侧会话存取。key 约定(README §5.6):
//   tok:{token}        = "uid:did"   (SETEX; access/refresh 双 token 各一条, 反向索引)
//   sess:{uid}:{did}   = access token(SETEX, 会话"活着"的证据, 撤销即删)
//   sess:user:{uid}    = set(did)    在线设备枚举
#pragma once
#include <cstdint>
#include <string>

namespace chatservice::cache {

// 签发后写入 Redis: 双 token 反向索引 + 会话存在性 + 在线设备集合
bool writeSession(uint64_t user_id, const std::string& device_id,
                  const std::string& access, const std::string& refresh,
                  long accessSec, long refreshSec);

// 凭 token 查 uid/did(反向索引 GET)。返回是否命中。
bool findUserByToken(const std::string& token, uint64_t* user_id,
                     std::string* device_id);

// 续期(滑动): 命中则把会话与 access 的 TTL 重置为 accessSec(不超过 refresh 上限)
bool touchSession(uint64_t user_id, const std::string& device_id,
                  const std::string& access, long accessSec);

// 撤销: 删 tok/sess 并移出在线设备集合
bool deleteSession(uint64_t user_id, const std::string& device_id,
                   const std::string& access, const std::string& refresh);

// 纯清除一条 token 反向索引(登出时兜底, 拿不到 uid/did 的场景)
void deleteTokenOnly(const std::string& token);

} // namespace chatservice::cache
