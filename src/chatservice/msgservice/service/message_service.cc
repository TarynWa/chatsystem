#include "msgservice/service/message_service.h"

#include "base/db_pool.h"
#include "base/snowflake.h"
#include "base/sqlutil.h"
#include "common/errcode.h"
#include "msgservice/dao/message_dao.h"
#include "msgservice/dao/offline_dao.h"

namespace chatservice::service {

namespace {
// 统一 clamp: default 在 (0, cap] 内由调用方给语义; <=0 用 def; > cap 用 cap
inline int clampLimit(int v, int def, int cap) {
  if (v <= 0) return def;
  return v > cap ? cap : v;
}
} // namespace

int sendMessage(uint64_t from_user, uint64_t to_user, int msg_type,
                const std::string& content, const std::string& media_urls,
                chatservice::model::SentMessage* out) {
  if (out) {
    out->msg_id = 0;
    out->created_at.clear();
  }
  if (from_user == 0 || to_user == 0) return ErrCode::ERR_PARAM;
  if (from_user == to_user) return ErrCode::ERR_MSG_SELF;

  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  chatservice::dao::Txn txn(db.c);
  if (!txn.ok()) return ErrCode::ERR_BUSY;

  uint64_t mid = chatservice::common::Snowflake::instance().nextId();
  const std::string table = chatservice::dao::currentMonthTable();

  int rc = chatservice::dao::insertMessage(db.c, table, mid, from_user, to_user,
                                           msg_type, content, media_urls);
  if (rc != ErrCode::OK) return rc;  // Txn 析构回滚
  rc = chatservice::dao::insertOffline(db.c, mid, to_user, from_user, msg_type,
                                       content, media_urls);
  if (rc != ErrCode::OK) return rc;

  std::string created;
  rc = chatservice::dao::queryCreatedAt(db.c, table, mid, &created);
  if (rc != ErrCode::OK) return rc;

  txn.commit();
  if (out) {
    out->msg_id = mid;
    out->created_at = created;
  }
  return ErrCode::OK;
}

int pullMessages(uint64_t user_id, int limit,
                 std::vector<chatservice::model::MessageRow>* out) {
  out->clear();
  if (user_id == 0) return ErrCode::ERR_PARAM;
  limit = clampLimit(limit, 50, 100);
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  return chatservice::dao::claimOffline(db.c, user_id, limit, out);
}

int getHistory(uint64_t me, uint64_t peer, uint64_t before_msg_id, int limit,
               std::vector<chatservice::model::MessageRow>* out, bool* has_more) {
  out->clear();
  if (has_more) *has_more = false;
  if (me == 0 || peer == 0 || me == peer) return ErrCode::ERR_PARAM;
  limit = clampLimit(limit, 20, 50);
  chatservice::dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;
  return chatservice::dao::fetchHistory(db.c, chatservice::dao::currentMonthTable(),
                                        me, peer, before_msg_id, limit, out, has_more);
}

} // namespace chatservice::service
