#include "authservice/service/session_service.h"

#include <cctype>
#include <cstdio>

#include "authservice/cache/fail_counter.h"
#include "authservice/cache/session_cache.h"
#include "authservice/dao/audit_dao.h"
#include "authservice/dao/db_pool.h"
#include "authservice/dao/device_dao.h"
#include "authservice/dao/session_dao.h"
#include "authservice/dao/user_dao.h"
#include "authservice/security/password_util.h"
#include "authservice/security/token_util.h"
#include "common/errcode.h"

namespace chatservice::service {

namespace {

// account 识别: 含@→email; 11位纯数字→phone; 否则 username
std::string classifyAccount(const std::string& account) {
  if (account.find('@') != std::string::npos) return "email";
  if (account.size() == 11) {
    bool allDigit = true;
    for (char ch : account)
      if (!std::isdigit((unsigned char)ch)) {
        allDigit = false;
        break;
      }
    if (allDigit) return "phone";
  }
  return "username";
}

// 会话签发核心(Login 与 注册自动登录共用), 持有单个 DB 连接做设备/会话多步写
int issueOnConn(MYSQL* db, uint64_t user_id, const std::string& username,
                const model::DeviceInfo& dev, const std::string& ip,
                const std::string& ua, const Policy& pol,
                model::SessionPair* tokens) {
  const std::string& did = dev.device_id;

  if (!dao::upsertDevice(db, user_id, dev)) return ErrCode::ERR_BUSY;

  bool curActive = dao::hasActiveSession(db, user_id, did);
  int others = dao::countActiveOthers(db, user_id, did);

  // 多端上限(§8.2 P7): 本设备无有效会话(=新设备)且总量将超上限才触发
  bool overLimit = !curActive && (others + 1 > pol.maxDevices);
  dao::SessionRow victim;
  bool haveVictim = false;
  if (overLimit) {
    if (!pol.kickOldest) return ErrCode::ERR_DEVICE_LIMIT;  // reject_new
    haveVictim = dao::pickOldestActiveOthers(db, user_id, did, &victim);
  }

  // 同设备重复登录 = 覆盖旧会话(幂等, 不重复计数)
  dao::deactivateByDevice(db, user_id, did);

  std::string at = security::genAccessToken();
  std::string rt = security::genRefreshToken();
  if (!dao::insertSession(db, user_id, did, at, rt, pol.accessSec,
                          pol.refreshSec, ip, ua))
    return ErrCode::ERR_BUSY;
  dao::updateLoginInfo(db, user_id, ip, dev.device_name + "|" + did);

  if (!cache::writeSession(user_id, did, at, rt, pol.accessSec, pol.refreshSec))
    return ErrCode::ERR_BUSY;

  // kick_oldest: 新会话立起来后再挤掉最久未活跃设备(该设备会话已在 Redis 失效)
  if (haveVictim) {
    dao::markRevoked(db, victim.id);
    dao::setDeviceOffline(db, user_id, victim.device_id);
    cache::deleteSession(user_id, victim.device_id, victim.token,
                         victim.refresh_token);
  }

  if (tokens) {
    tokens->access_token = at;
    tokens->access_expires_in = (int)pol.accessSec;
    tokens->refresh_token = rt;
    tokens->refresh_expires_in = (int)pol.refreshSec;
  }
  return ErrCode::OK;
}

} // namespace

int login(const std::string& account, const std::string& password,
          const model::DeviceInfo& dev, const std::string& ip,
          const std::string& ua, const Policy& pol, model::UserRow* user,
          model::SessionPair* tokens) {
  dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;

  // P2 锁检查
  if (cache::failLocked(account)) return ErrCode::ERR_USER_LOCKED;

  // P3/P4 找账号 & 软删除
  model::UserRow u = dao::findUserBy(db.c, classifyAccount(account), account);
  if (!u.found) {
    dao::addAudit(0, "", "login", "account", account, "", ip, ua, false,
                  "account not exist");
    return ErrCode::ERR_USER_NOT_EXIST;
  }
  if (u.deleted) {
    dao::addAudit(u.id, u.username, "login", "user", std::to_string(u.id), "",
                  ip, ua, false, "user deleted");
    return ErrCode::ERR_USER_DELETED;
  }

  // P5 验密
  if (!security::verifyPassword(password, u.password_hash)) {
    cache::recordFail(account, pol.maxFail, pol.lockMinutes);
    bool nowLocked = cache::failLocked(account);
    dao::addAudit(u.id, u.username, "login", "user", std::to_string(u.id), "",
                  ip, ua, false, "wrong password");
    return nowLocked ? ErrCode::ERR_USER_LOCKED : ErrCode::ERR_PWD_ERROR;
  }

  // P6 成功清计数
  cache::clearFail(account);

  // P7/P8 多端检查 + 会话签发
  int rc = issueOnConn(db.c, u.id, u.username, dev, ip, ua, pol, tokens);
  if (rc != ErrCode::OK) {
    dao::addAudit(u.id, u.username, "login", "user", std::to_string(u.id), "",
                  ip, ua, false, "issue session failed");
    return rc;
  }

  if (user) *user = u;
  dao::addAudit(u.id, u.username, "login", "user", std::to_string(u.id), "",
                ip, ua, true, "");
  return ErrCode::OK;
}

int autoLogin(uint64_t user_id, const std::string& username,
              const model::DeviceInfo& dev, const std::string& ip,
              const std::string& ua, const Policy& pol,
              model::SessionPair* tokens) {
  dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  int rc = issueOnConn(db.c, user_id, username, dev, ip, ua, pol, tokens);
  if (rc == ErrCode::OK)
    dao::addAudit(user_id, username, "login", "user",
                  std::to_string(user_id), "", ip, ua, true, "auto login");
  return rc;
}

int verifyToken(const std::string& token, const Policy& pol,
                uint64_t* user_id, std::string* device_id) {
  if (token.empty()) return ErrCode::ERR_TOKEN_INVALID;
  uint64_t uid = 0;
  std::string did;
  if (!cache::findUserByToken(token, &uid, &did))
    return ErrCode::ERR_TOKEN_INVALID;
  if (user_id) *user_id = uid;
  if (device_id) *device_id = did;
  // 滑动续期(§5.2): 会话与 token TTL 重置为 access 有效期
  cache::touchSession(uid, did, token, pol.accessSec);
  return ErrCode::OK;
}

int logout(const std::string& token, uint64_t* user_id,
           std::string* device_id) {
  uint64_t outUid = 0;
  std::string outDid;
  // 先反查归属(即使 DB 行已丢, 也尽量清干净)
  bool known = cache::findUserByToken(token, &outUid, &outDid);

  dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;

  dao::SessionRow row;
  if (dao::findActiveByToken(db.c, token, &row)) {
    outUid = row.user_id;
    outDid = row.device_id;
    dao::markRevoked(db.c, row.id);
    dao::setDeviceOffline(db.c, row.user_id, row.device_id);
    cache::deleteSession(row.user_id, row.device_id, row.token,
                         row.refresh_token);
  } else if (known) {
    cache::deleteTokenOnly(token);
  }

  dao::addAudit(outUid, "", "logout", "session", token, "", "", "", true, "");
  if (user_id) *user_id = outUid;
  if (device_id) *device_id = outDid;
  return ErrCode::OK;  // 幂等: 不存在的 token 视为已登出
}

} // namespace chatservice::service
