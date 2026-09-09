// SocialServiceRpc 实现(S0 好友+黑名单): 参数校验 + 调 service, 不直接写 SQL。
#pragma once
#include "social.pb.h"

namespace chatservice {

class SocialServiceImpl : public SocialServiceRpc {
 public:
  void AddFriend(::google::protobuf::RpcController* controller,
                 const ::chatservice::AddFriendRequest* request,
                 ::chatservice::AddFriendResponse* response,
                 ::google::protobuf::Closure* done) override;
  void HandleFriend(::google::protobuf::RpcController* controller,
                    const ::chatservice::HandleFriendRequest* request,
                    ::chatservice::HandleFriendResponse* response,
                    ::google::protobuf::Closure* done) override;
  void ListFriends(::google::protobuf::RpcController* controller,
                   const ::chatservice::ListFriendsRequest* request,
                   ::chatservice::ListFriendsResponse* response,
                   ::google::protobuf::Closure* done) override;
  void DeleteFriend(::google::protobuf::RpcController* controller,
                    const ::chatservice::DeleteFriendRequest* request,
                    ::chatservice::DeleteFriendResponse* response,
                    ::google::protobuf::Closure* done) override;
  void AddBlacklist(::google::protobuf::RpcController* controller,
                    const ::chatservice::AddBlacklistRequest* request,
                    ::chatservice::AddBlacklistResponse* response,
                    ::google::protobuf::Closure* done) override;
  void ListBlacklist(::google::protobuf::RpcController* controller,
                     const ::chatservice::ListBlacklistRequest* request,
                     ::chatservice::ListBlacklistResponse* response,
                     ::google::protobuf::Closure* done) override;
  void RemoveBlacklist(::google::protobuf::RpcController* controller,
                       const ::chatservice::RemoveBlacklistRequest* request,
                       ::chatservice::RemoveBlacklistResponse* response,
                       ::google::protobuf::Closure* done) override;

 private:
  static void setResult(Result* r, int code);
};

} // namespace chatservice
