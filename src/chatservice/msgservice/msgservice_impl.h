// MsgServiceRpc 实现(S0 单聊): 参数校验 + 调 service, 不直接写 SQL(见 README §层红线)
#pragma once
#include "msg.pb.h"

namespace chatservice {

class MsgServiceImpl : public MsgServiceRpc {
 public:
  void SendMsg(::google::protobuf::RpcController* controller,
               const ::chatservice::SendMsgRequest* request,
               ::chatservice::SendMsgResponse* response,
               ::google::protobuf::Closure* done) override;
  void PullNewMsg(::google::protobuf::RpcController* controller,
                  const ::chatservice::PullNewMsgRequest* request,
                  ::chatservice::PullNewMsgResponse* response,
                  ::google::protobuf::Closure* done) override;
  void GetHistory(::google::protobuf::RpcController* controller,
                  const ::chatservice::GetHistoryRequest* request,
                  ::chatservice::GetHistoryResponse* response,
                  ::google::protobuf::Closure* done) override;

 private:
  static void setResult(Result* r, int code);
};

} // namespace chatservice
