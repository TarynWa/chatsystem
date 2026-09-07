// RPC 实现层(AuthServiceRpc): 协议编解码 + 参数校验 → 调 service 层。
// 不直接写 SQL / 不直接碰 Redis(见 README §7 层职责红线)。
#pragma once
#include "auth.pb.h"
#include "authservice/model/user.h"
#include "authservice/service/session_service.h"

namespace chatservice {

class AuthServiceImpl final : public AuthServiceRpc {
 public:
  explicit AuthServiceImpl(service::Policy pol) : pol_(pol) {}

  void Register(::google::protobuf::RpcController* controller,
                const ::chatservice::RegisterRequest* request,
                ::chatservice::RegisterResponse* response,
                ::google::protobuf::Closure* done) override;

  void Login(::google::protobuf::RpcController* controller,
             const ::chatservice::LoginRequest* request,
             ::chatservice::LoginResponse* response,
             ::google::protobuf::Closure* done) override;

  void VerifyToken(::google::protobuf::RpcController* controller,
                   const ::chatservice::VerifyTokenRequest* request,
                   ::chatservice::VerifyTokenResponse* response,
                   ::google::protobuf::Closure* done) override;

  void Logout(::google::protobuf::RpcController* controller,
              const ::chatservice::LogoutRequest* request,
              ::chatservice::LogoutResponse* response,
              ::google::protobuf::Closure* done) override;

 private:
  service::Policy pol_;

  static void setResult(Result* r, int code);
  static void setBrief(UserBrief* ub, uint64_t uid, const std::string& username,
                       const std::string& nickname, int status);
};

} // namespace chatservice
