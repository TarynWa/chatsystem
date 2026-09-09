#include "socialservice/service/social_service.h"

#include "base/db_pool.h"
#include "base/sqlutil.h"
#include "common/errcode.h"
#include "socialservice/dao/black_dao.h"
#include "socialservice/dao/friend_dao.h"
#include "socialservice/dao/request_dao.h"

namespace chatservice::service {

int addFriend(uint64_t from_user, uint64_t to_user, const std::string& apply_msg,
              uint64_t* request_id, std::string* expire_at) {
  if (request_id) *request_id = 0;
  if (expire_at) expire_at->clear();
  if (from_user == 0 || to_user == 0) return ErrCode::ERR_PARAM;
  if (from_user == to_user) return ErrCode::ERR_SELF_OP;

  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  if (chatservice::dao::isFriend(db.c, from_user, to_user))
    return ErrCode::ERR_ALREADY_FRIEND;

  bool pending = false;
  int rc = chatservice::dao::hasPendingRequest(db.c, from_user, to_user, &pending);
  if (rc != ErrCode::OK) return rc;
  if (pending) return ErrCode::ERR_REQUEST_PENDING;

  if (chatservice::dao::isBlocked(db.c, to_user, from_user))
    return ErrCode::ERR_BLOCKED_BY_PEER;  // 你在对方黑名单
  if (chatservice::dao::isBlocked(db.c, from_user, to_user))
    return ErrCode::ERR_BLOCKED_PEER;  // 你已拉黑对方

  return chatservice::dao::insertRequest(db.c, from_user, to_user, apply_msg,
                                         request_id, expire_at);
}

int handleFriendRequest(uint64_t handler_id, uint64_t request_id, int action) {
  if (handler_id == 0 || request_id == 0) return ErrCode::ERR_PARAM;
  if (action != 1 && action != 2) return ErrCode::ERR_PARAM;

  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;

  chatservice::model::RequestRow req;
  bool found = false;
  int rc = chatservice::dao::loadRequest(db.c, request_id, &req, &found);
  if (rc != ErrCode::OK) return rc;
  if (!found) return ErrCode::ERR_REQUEST_INVALID;
  if (req.to_user != handler_id) return ErrCode::ERR_REQUEST_FORBIDDEN;
  if (req.status != 0) return ErrCode::ERR_REQUEST_INVALID;  // 已处理/已标记过期
  if (chatservice::dao::requestExpired(db.c, request_id)) {
    chatservice::dao::markRequestExpired(db.c, request_id);  // 过期标记, 失败忽略
    return ErrCode::ERR_REQUEST_EXPIRED;
  }

  if (action == 2) {  // 拒绝: 单语句置 2(护栏并发: 抢跑者生效, 后者也返回 0)
    int affected = 0;
    return chatservice::dao::rejectRequest(db.c, request_id, handler_id, &affected);
  }

  // 接受: 同一事务内 置 status=1 → 双向建/激活 friends(status=1) → commit
  chatservice::dao::Txn txn(db.c);
  if (!txn.ok()) return ErrCode::ERR_BUSY;
  int affected = 0;
  rc = chatservice::dao::acceptRequest(db.c, request_id, handler_id, &affected);
  if (rc != ErrCode::OK) return rc;
  if (affected == 0) return ErrCode::ERR_REQUEST_INVALID;  // 并发抢先处理/恰过期, 回滚

  rc = chatservice::dao::upsertFriend(db.c, req.from_user, req.to_user,
                                      req.apply_msg);  // 申请人视角行
  if (rc != ErrCode::OK) return rc;
  rc = chatservice::dao::upsertFriend(db.c, req.to_user, req.from_user,
                                      req.apply_msg);  // 处理人视角行
  if (rc != ErrCode::OK) return rc;
  txn.commit();
  return ErrCode::OK;
}

int deleteFriend(uint64_t user_id, uint64_t friend_id) {
  if (user_id == 0 || friend_id == 0) return ErrCode::ERR_PARAM;
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  int rc = chatservice::dao::softDeleteFriend(db.c, user_id, friend_id);
  if (rc != ErrCode::OK) return rc;
  return chatservice::dao::softDeleteFriend(db.c, friend_id, user_id);
}

int listFriends(uint64_t user_id,
                std::vector<chatservice::model::FriendRow>* out) {
  out->clear();
  if (user_id == 0) return ErrCode::ERR_PARAM;
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  return chatservice::dao::listFriends(db.c, user_id, out);
}

int addBlacklist(uint64_t user_id, uint64_t blocked_user_id,
                 const std::string& reason) {
  if (user_id == 0 || blocked_user_id == 0) return ErrCode::ERR_PARAM;
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  return chatservice::dao::addBlack(db.c, user_id, blocked_user_id, reason);
}

int removeBlacklist(uint64_t user_id, uint64_t blocked_user_id) {
  if (user_id == 0 || blocked_user_id == 0) return ErrCode::ERR_PARAM;
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  return chatservice::dao::removeBlack(db.c, user_id, blocked_user_id);
}

int listBlacklist(uint64_t user_id,
                  std::vector<chatservice::model::BlackRow>* out) {
  out->clear();
  if (user_id == 0) return ErrCode::ERR_PARAM;
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  return chatservice::dao::listBlack(db.c, user_id, out);
}

} // namespace chatservice::service
