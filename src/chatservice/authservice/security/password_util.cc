#include "password_util.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace chatservice::security {

namespace {
// /dev/urandom 读 n 字节 -> 16 进制串
std::string randomHex(int n) {
  std::string out;
  out.reserve(n * 2);
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f) return out;
  unsigned char buf[32];
  while (n > 0) {
    int chunk = n > 32 ? 32 : n;
    if (fread(buf, 1, chunk, f) != (size_t)chunk) break;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < chunk; ++i) {
      out.push_back(hex[buf[i] >> 4]);
      out.push_back(hex[buf[i] & 0x0F]);
    }
    n -= chunk;
  }
  fclose(f);
  return out;
}

uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string toHex(uint64_t v) {
  static const char* hex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = hex[v & 0xF];
    v >>= 4;
  }
  return out;
}

bool constEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char r = 0;
  for (size_t i = 0; i < a.size(); ++i) r |= (unsigned char)(a[i] ^ b[i]);
  return r == 0;
}
} // namespace

std::string hashPassword(const std::string& plain) {
  std::string salt = randomHex(4); // 8 字符盐(占位; bcrypt 盐为 22 字符)
  return salt + "$" + toHex(fnv1a64(salt + plain));
}

bool verifyPassword(const std::string& plain, const std::string& stored) {
  size_t pos = stored.find('$');
  if (pos == std::string::npos) return false;
  std::string salt = stored.substr(0, pos);
  std::string calc = salt + "$" + toHex(fnv1a64(salt + plain));
  return constEq(calc, stored);
}

} // namespace chatservice::security
