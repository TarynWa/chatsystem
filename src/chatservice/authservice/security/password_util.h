// 密码哈希 —— S0 占位实现(FNV-1a 64 + 固定盐)，仅保证"不落明文"。
// ⚠ 非加密安全：M2 按自研度决策替换为 bcrypt(cost=10)，见 README §5.1 / §14。
#pragma once
#include <string>

namespace chatservice::security {

// 计算 FNV-1a 64 位哈希并转 16 进制，与随机盐拼为 "盐$哈希"
std::string hashPassword(const std::string& plain);

// 校验: 用存储串里的盐重新计算比较(常数时间), 兼容未来切换算法
bool verifyPassword(const std::string& plain, const std::string& stored);

} // namespace chatservice::security
