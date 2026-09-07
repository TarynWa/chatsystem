// token 生成 —— S0 占位实现(随机串)。M2 起 access 换 JWT HS256、refresh 轮换。
// 形如 "at_" / "rt_" + 32B urandom hex；随机串便于服务端吊销(存 Redis/MySQL)。
#pragma once
#include <string>

namespace chatservice::security {

// 生成 access token（前缀 at_）
std::string genAccessToken();

// 生成 refresh token（前缀 rt_）
std::string genRefreshToken();

} // namespace chatservice::security
