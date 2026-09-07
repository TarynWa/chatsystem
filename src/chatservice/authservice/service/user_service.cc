#include "authservice/service/user_service.h"

#include "authservice/dao/db_pool.h"
#include "authservice/dao/user_dao.h"
#include "common/errcode.h"

namespace chatservice::service {

int createUser(const NewUser& u, uint64_t* newId) {
  dao::DbConn db;
  if (!db.ok()) return ErrCode::ERR_BUSY;

  // 先查后插: 常规路径直接给 1101; 并发下由 INSERT 唯一键(1062)兜底
  if (dao::findUserBy(db.c, "username", u.username).found)
    return ErrCode::ERR_USER_EXIST;
  return dao::insertUser(db.c, u.username, u.password_hash, u.nickname,
                         u.email, u.phone, newId);
}

} // namespace chatservice::service
