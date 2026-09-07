#include "authservice/cache/session_cache.h"

#include <cstdio>
#include <cstdlib>

#include "authservice/cache/redis_pool.h"

namespace chatservice::cache {

namespace {
std::string tokKey(const std::string& token) { return "tok:" + token; }
std::string sessKey(uint64_t uid, const std::string& did) {
  return "sess:" + std::to_string(uid) + ":" + did;
}
std::string onlineKey(uint64_t uid) { return "sess:user:" + std::to_string(uid); }

// reply 是 string 类型则取出
std::string replyString(redisReply* r) {
  return (r && r->type == REDIS_REPLY_STRING && r->str)
             ? std::string(r->str, r->len)
             : std::string();
}

long long replyInt(redisReply* r) {
  return (r && r->type == REDIS_REPLY_INTEGER) ? r->integer : -1;
}
} // namespace

bool writeSession(uint64_t user_id, const std::string& device_id,
                  const std::string& access, const std::string& refresh,
                  long accessSec, long refreshSec) {
  RedisConn rc;
  if (!rc.ok()) return false;
  std::string uidDid = std::to_string(user_id) + ":" + device_id;
  bool ok = true;
  redisReply* r = RedisPool::run(
      rc.c, {"SETEX", tokKey(access), std::to_string(accessSec), uidDid});
  if (r) freeReplyObject(r); else ok = false;
  r = RedisPool::run(
      rc.c, {"SETEX", tokKey(refresh), std::to_string(refreshSec), uidDid});
  if (r) freeReplyObject(r); else ok = false;
  r = RedisPool::run(rc.c, {"SETEX", sessKey(user_id, device_id),
                            std::to_string(accessSec), access});
  if (r) freeReplyObject(r); else ok = false;
  r = RedisPool::run(rc.c, {"SADD", onlineKey(user_id), device_id});
  if (r) freeReplyObject(r); else ok = false;
  return ok;
}

bool findUserByToken(const std::string& token, uint64_t* user_id,
                     std::string* device_id) {
  RedisConn rc;
  if (!rc.ok()) return false;
  redisReply* r = RedisPool::run(rc.c, {"GET", tokKey(token)});
  std::string v = replyString(r);
  if (r) freeReplyObject(r);
  if (v.empty()) return false;
  size_t pos = v.find(':');
  if (pos == std::string::npos) return false;
  uint64_t uid = strtoull(v.substr(0, pos).c_str(), nullptr, 10);
  if (uid == 0) return false;
  if (user_id) *user_id = uid;
  if (device_id) *device_id = v.substr(pos + 1);
  return true;
}

bool touchSession(uint64_t user_id, const std::string& device_id,
                  const std::string& access, long accessSec) {
  RedisConn rc;
  if (!rc.ok()) return false;
  redisReply* r =
      RedisPool::run(rc.c, {"EXPIRE", sessKey(user_id, device_id),
                            std::to_string(accessSec)});
  if (r) freeReplyObject(r);
  r = RedisPool::run(
      rc.c, {"EXPIRE", tokKey(access), std::to_string(accessSec)});
  if (r) freeReplyObject(r);
  return true;
}

bool deleteSession(uint64_t user_id, const std::string& device_id,
                   const std::string& access, const std::string& refresh) {
  RedisConn rc;
  if (!rc.ok()) return false;
  std::vector<std::string> del = {"DEL", tokKey(access), tokKey(refresh),
                                  sessKey(user_id, device_id)};
  redisReply* r = RedisPool::run(rc.c, del);
  if (r) freeReplyObject(r);
  r = RedisPool::run(rc.c, {"SREM", onlineKey(user_id), device_id});
  if (r) freeReplyObject(r);
  return true;
}

void deleteTokenOnly(const std::string& token) {
  RedisConn rc;
  if (!rc.ok()) return;
  redisReply* r = RedisPool::run(rc.c, {"DEL", tokKey(token)});
  if (r) freeReplyObject(r);
}

} // namespace chatservice::cache
