#include "token_util.h"

#include <cstdio>

namespace chatservice::security {

namespace {
std::string randomHex(int n) {
  std::string out;
  out.reserve(n * 2);
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f) return out;
  unsigned char buf[32];
  static const char* hex = "0123456789abcdef";
  while (n > 0) {
    int chunk = n > 32 ? 32 : n;
    if (fread(buf, 1, chunk, f) != (size_t)chunk) break;
    for (int i = 0; i < chunk; ++i) {
      out.push_back(hex[buf[i] >> 4]);
      out.push_back(hex[buf[i] & 0x0F]);
    }
    n -= chunk;
  }
  fclose(f);
  return out;
}
} // namespace

std::string genAccessToken() {
  return "at_" + randomHex(32);
}

std::string genRefreshToken() {
  return "rt_" + randomHex(32);
}

} // namespace chatservice::security
