// 字段级校验(本地先判, DB 兜底)。全部纯 ASCII 判断, 不依赖 <regex>。
#pragma once
#include <cctype>
#include <string>

namespace chatservice::validate {

// username: 3~32, 首字符字母, 后续字母/数字/_
inline bool username(const std::string& s) {
  if (s.size() < 3 || s.size() > 32) return false;
  if (!std::isalpha((unsigned char)s[0])) return false;
  for (char ch : s)
    if (!std::isalnum((unsigned char)ch) && ch != '_') return false;
  return true;
}

// password: 8~32, 须同时含字母与数字
inline bool password(const std::string& s) {
  if (s.size() < 8 || s.size() > 32) return false;
  bool hasLetter = false, hasDigit = false;
  for (char ch : s) {
    if (std::isalpha((unsigned char)ch)) hasLetter = true;
    if (std::isdigit((unsigned char)ch)) hasDigit = true;
  }
  return hasLetter && hasDigit;
}

// email: 空串也合法(未绑定); 非空须含@, @后有点, 无空白; 长度≤128
inline bool email(const std::string& s) {
  if (s.empty()) return true;
  if (s.size() > 128) return false;
  for (char ch : s)
    if (std::isspace((unsigned char)ch)) return false;
  size_t at = s.find('@');
  if (at == std::string::npos) return false;
  return s.find('.', at + 1) != std::string::npos;
}

// phone: 空串也合法(未绑定); 非空须 6~20 位数字
inline bool phone(const std::string& s) {
  if (s.empty()) return true;
  if (s.size() < 6 || s.size() > 20) return false;
  for (char ch : s)
    if (!std::isdigit((unsigned char)ch)) return false;
  return true;
}

// device_id: ^[A-Za-z0-9_.-]{1,64}$ —— 会拼进 Redis key / SQL, 必须收紧字符集
inline bool deviceId(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char ch : s)
    if (!std::isalnum((unsigned char)ch) && ch != '_' && ch != '.' && ch != '-' &&
        ch != ':')
      return false;
  return true;
}

// device_name / nickname 之类仅限长
inline bool lengthOk(const std::string& s, size_t max) {
  return s.size() <= max;
}

} // namespace chatservice::validate
