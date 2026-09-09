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
  // 消息(14xx)
  ERR_MSG_SELF = 1401,      // 不能给自己发消息
  // 社交(15xx)
  ERR_ALREADY_FRIEND = 1501,   // 已是好友
  ERR_REQUEST_PENDING = 1502,  // 已存在未处理申请
  ERR_REQUEST_INVALID = 1503,  // 申请不存在或已处理
  ERR_REQUEST_EXPIRED = 1504,  // 申请已过期
  ERR_REQUEST_FORBIDDEN = 1505, // 无权处理(非接收人)
  ERR_SELF_OP = 1506,      // 不能对自己操作
  ERR_BLOCKED_BY_PEER = 1507,  // 你在对方黑名单
  ERR_BLOCKED_PEER = 1508,     // 你已拉黑对方
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
    case ERR_MSG_SELF: return "cannot send message to self";
    case ERR_ALREADY_FRIEND: return "already friends";
    case ERR_REQUEST_PENDING: return "request already pending";
    case ERR_REQUEST_INVALID: return "request not exist or already handled";
    case ERR_REQUEST_EXPIRED: return "request expired";
    case ERR_REQUEST_FORBIDDEN: return "not allowed to handle this request";
    case ERR_SELF_OP: return "cannot operate on self";
    case ERR_BLOCKED_BY_PEER: return "you are blocked by the peer";
    case ERR_BLOCKED_PEER: return "you have blocked the peer";
    default: return "unknown error";
  }
}

} // namespace chatservice
