// 认证服务错误码规范（对应 README §5.7）
// 0 OK / 1xxx 公共 / 11xx 用户 / 12xx 会话
#pragma once
#include <string>

namespace chatservice {

enum ErrCode {
  OK = 0,
  // 公共
  ERR_PARAM = 1001,   // 参数缺失/越界/格式(本地判)
  ERR_BUSY = 1002,    // 服务繁忙 / DB 异常兜底
  ERR_FREQ = 1003,    // 操作频繁
  // 用户
  ERR_USER_EXIST = 1101,   // 用户名已存在
  ERR_USER_NOT_EXIST = 1102, // 账号不存在
  ERR_PWD_ERROR = 1103,    // 密码错误
  ERR_USER_LOCKED = 1104,  // 账号锁定(防爆破, M3 启用)
  ERR_USER_DELETED = 1105, // 用户已删除(软删除)
  ERR_BAD_ACCOUNT = 1106,  // 用户名或密码格式非法
  // 会话
  ERR_TOKEN_INVALID = 1201, // token 无效或已吊销
  ERR_TOKEN_EXPIRED = 1202, // access 过期(暂未区分)
  ERR_DEVICE_LIMIT = 1204,  // 设备数超上限
};

// 错误码 -> 人读文案(回填 Result.msg)
inline const char* ErrText(int code) {
  switch (code) {
    case OK: return "ok";
    case ERR_PARAM: return "invalid parameter";
    case ERR_BUSY: return "service busy";
    case ERR_FREQ: return "too many requests";
    case ERR_USER_EXIST: return "username already exists";
    case ERR_USER_NOT_EXIST: return "account not exist";
    case ERR_PWD_ERROR: return "wrong password";
    case ERR_USER_LOCKED: return "account locked";
    case ERR_USER_DELETED: return "user deleted";
    case ERR_BAD_ACCOUNT: return "invalid account or password format";
    case ERR_TOKEN_INVALID: return "token invalid or revoked";
    case ERR_TOKEN_EXPIRED: return "token expired";
    case ERR_DEVICE_LIMIT: return "device limit reached";
    default: return "unknown error";
  }
}

} // namespace chatservice
