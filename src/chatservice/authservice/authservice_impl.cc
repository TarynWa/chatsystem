#include "authservice/authservice_impl.h"

#include <string>

#include "authservice/dao/audit_dao.h"
#include "authservice/model/user.h"
#include "authservice/security/password_util.h"
#include "authservice/service/user_service.h"
#include "common/errcode.h"
#include "common/validate.h"

namespace chatservice {

namespace {
// proto DeviceInfo(chatservice::DeviceInfo) → 领域 model::DeviceInfo(dao/service 层契约)
model::DeviceInfo toModelDevice(const chatservice::DeviceInfo& d) {
  model::DeviceInfo m;
  m.device_id = d.device_id();
  m.device_name = d.device_name();
  m.device_type = d.device_type();
  m.os_version = d.os_version();
  m.app_version = d.app_version();
  return m;
}
} // namespace

void AuthServiceImpl::setResult(Result* r, int code) {
  r->set_code(code);
  r->set_msg(ErrText(code));
}

void AuthServiceImpl::setBrief(UserBrief* ub, uint64_t uid,
                               const std::string& username,
                               const std::string& nickname, int status) {
  ub->set_user_id(uid);
  ub->set_username(username);
  ub->set_nickname(nickname);
  ub->set_avatar("");
  ub->set_status(status);
}

void AuthServiceImpl::Register(
    ::google::protobuf::RpcController* controller,
    const ::chatservice::RegisterRequest* request,
    ::chatservice::RegisterResponse* response,
    ::google::protobuf::Closure* done) {
  const std::string& username = request->username();
  const std::string& password = request->password();
  std::string nickname = request->nickname();
  const std::string& email = request->email();
  const std::string& phone = request->phone();
  const bool hasDevice = !request->device().device_id().empty();

  // ---- 本地校验(README §8.1 P1/P2) ----
  if (username.empty() || password.empty() || (hasDevice && !validate::deviceId(request->device().device_id()))) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (nickname.empty()) nickname = username;
  if (!validate::username(username) || !validate::password(password)) {
    setResult(response->mutable_result(), ErrCode::ERR_BAD_ACCOUNT);
    done->Run();
    return;
  }
  if (nickname.size() > 32 || !validate::email(email) || !validate::phone(phone) ||
      !validate::lengthOk(request->device().device_name(), 64) ||
      !validate::lengthOk(request->device().os_version(), 64) ||
      !validate::lengthOk(request->device().app_version(), 32)) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }

  // ---- 落库 ----
  service::NewUser nu;
  nu.username = username;
  nu.password_hash = security::hashPassword(password);  // 占位哈希, M2 换 bcrypt
  nu.nickname = nickname;
  nu.email = email;
  nu.phone = phone;
  uint64_t uid = 0;
  int rc = service::createUser(nu, &uid);
  dao::addAudit(rc == ErrCode::OK ? uid : 0, username, "register", "user",
                std::to_string(uid), "", "", "", rc == ErrCode::OK,
                ErrText(rc));
  if (rc != ErrCode::OK) {
    setResult(response->mutable_result(), rc);
    done->Run();
    return;
  }

  response->set_user_id(uid);
  setResult(response->mutable_result(), ErrCode::OK);
  setBrief(response->mutable_user(), uid, username, nickname, 0);

  // ---- 注册成功默认自动登录(§8.1) ----
  if (hasDevice) {
    model::SessionPair tokens;
    model::DeviceInfo dev = toModelDevice(request->device());
    int ar = service::autoLogin(uid, username, dev, "", "", pol_, &tokens);
    if (ar == ErrCode::OK) {
      TokenPair* tp = response->mutable_tokens();
      tp->set_access_token(tokens.access_token);
      tp->set_access_expires_in(tokens.access_expires_in);
      tp->set_refresh_token(tokens.refresh_token);
      tp->set_refresh_expires_in(tokens.refresh_expires_in);
    } else {
      // 用户已建但自动登录失败(如设备超限): 仍可走普通登录; 结果码给自动登录的
      setResult(response->mutable_result(), ar);
    }
  }
  done->Run();
}

void AuthServiceImpl::Login(::google::protobuf::RpcController* controller,
                            const ::chatservice::LoginRequest* request,
                            ::chatservice::LoginResponse* response,
                            ::google::protobuf::Closure* done) {
  const std::string& account = request->account();
  const std::string& password = request->password();

  if (account.empty() || account.size() > 128 ||
      request->device().device_id().empty() ||
      !validate::deviceId(request->device().device_id())) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (password.empty() || password.size() > 32) {
    setResult(response->mutable_result(), ErrCode::ERR_BAD_ACCOUNT);
    done->Run();
    return;
  }
  if (!validate::lengthOk(request->device().device_name(), 64) ||
      !validate::lengthOk(request->device().os_version(), 64) ||
      !validate::lengthOk(request->device().app_version(), 32) ||
      !validate::lengthOk(request->user_agent(), 255) ||
      !validate::lengthOk(request->ip(), 45)) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }

  model::UserRow user;
  model::SessionPair tokens;
  model::DeviceInfo dev = toModelDevice(request->device());
  int rc = service::login(account, password, dev, request->ip(),
                          request->user_agent(), pol_, &user, &tokens);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    TokenPair* tp = response->mutable_tokens();
    tp->set_access_token(tokens.access_token);
    tp->set_access_expires_in(tokens.access_expires_in);
    tp->set_refresh_token(tokens.refresh_token);
    tp->set_refresh_expires_in(tokens.refresh_expires_in);
    setBrief(response->mutable_user(), user.id, user.username, user.nickname,
             user.status);
  }
  done->Run();
}

void AuthServiceImpl::VerifyToken(
    ::google::protobuf::RpcController* controller,
    const ::chatservice::VerifyTokenRequest* request,
    ::chatservice::VerifyTokenResponse* response,
    ::google::protobuf::Closure* done) {
  uint64_t uid = 0;
  std::string did;
  int rc = service::verifyToken(request->token(), pol_, &uid, &did);
  setResult(response->mutable_result(), rc == ErrCode::OK ? ErrCode::OK
                                                          : ErrCode::ERR_TOKEN_INVALID);
  response->set_valid(rc == ErrCode::OK);
  if (rc == ErrCode::OK) {
    response->set_user_id(uid);
    response->set_device_id(did);
  }
  done->Run();
}

void AuthServiceImpl::Logout(::google::protobuf::RpcController* controller,
                             const ::chatservice::LogoutRequest* request,
                             ::chatservice::LogoutResponse* response,
                             ::google::protobuf::Closure* done) {
  uint64_t uid = 0;
  std::string did;
  int rc = service::logout(request->token(), &uid, &did);  // 幂等
  setResult(response->mutable_result(), rc);
  done->Run();
}

} // namespace chatservice
