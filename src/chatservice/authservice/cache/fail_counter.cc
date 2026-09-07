#include "authservice/cache/fail_counter.h"

#include "authservice/cache/redis_pool.h"

namespace chatservice::cache {

namespace {
std::string failKey(const std::string& account) {
  return "login:fail:" + account;
}
std::string lockKey(const std::string& account) {
  return "login:lock:" + account;
}
long long replyInt(redisReply* r) {
  return (r && r->type == REDIS_REPLY_INTEGER) ? r->integer : 0;
}
} // namespace

bool failLocked(const std::string& account) {
  RedisConn rc;
  if (!rc.ok()) return false;
  redisReply* r = RedisPool::run(rc.c, {"EXISTS", lockKey(account)});
  bool locked = r && r->type == REDIS_REPLY_INTEGER && r->integer >= 1;
  if (r) freeReplyObject(r);
  return locked;
}

bool recordFail(const std::string& account, int maxFail, int lockMinutes) {
  RedisConn rc;
  if (!rc.ok()) return false;
  redisReply* r = RedisPool::run(rc.c, {"INCR", failKey(account)});
  long long n = replyInt(r);
  if (r) freeReplyObject(r);
  // 计数 key 本身保持一个滑动窗口(TTL 30min)让计数逐步归零
  r = RedisPool::run(rc.c, {"EXPIRE", failKey(account), "1800"});
  if (r) freeReplyObject(r);
  if (n < maxFail) return false;
  long ttl = lockMinutes * 60L;
  r = RedisPool::run(rc.c, {"SET", lockKey(account), "1", "EX",
                            std::to_string(ttl)});
  bool newly = r && r->type == REDIS_REPLY_STATUS;
  if (r) freeReplyObject(r);
  return newly;
}

void clearFail(const std::string& account) {
  RedisConn rc;
  if (!rc.ok()) return;
  redisReply* r = RedisPool::run(rc.c, {"DEL", failKey(account), lockKey(account)});
  if (r) freeReplyObject(r);
}

} // namespace chatservice::cache
