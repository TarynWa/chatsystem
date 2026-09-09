// SocialServiceRpc 实现(S0 好友+黑名单): 参数校验 + 调 service, 不直接写 SQL(见 README §层红线)
#include "socialservice/socialservice_impl.h"

#include "common/errcode.h"
#include "common/validate.h"
#include "socialservice/model/social.h"
#include "socialservice/service/social_service.h"

namespace chatservice {

namespace {
// S0 长度上限(均对应表列类型)
constexpr size_t kMaxApplyMsg = 255;
constexpr size_t kMaxReason = 255;
} // namespace

void SocialServiceImpl::setResult(Result* r, int code) {
  r->set_code(code);
  r->set_msg(ErrText(code));
}

void SocialServiceImpl::AddFriend(::google::protobuf::RpcController* controller,
                                  const ::chatservice::AddFriendRequest* request,
                                  ::chatservice::AddFriendResponse* response,
                                  ::google::protobuf::Closure* done) {
  const uint64_t from = request->from_user();
  const uint64_t to = request->to_user();
  if (from == 0 || to == 0 || request->apply_msg().size() > kMaxApplyMsg) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (from == to) {
    setResult(response->mutable_result(), ErrCode::ERR_SELF_OP);
    done->Run();
    return;
  }
  uint64_t request_id = 0;
  std::string expire_at;
  int rc = service::addFriend(from, to, request->apply_msg(), &request_id,
                              &expire_at);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    response->set_request_id(request_id);
    response->set_expire_at(expire_at);
  }
  done->Run();
}

void SocialServiceImpl::HandleFriend(::google::protobuf::RpcController* controller,
                                     const ::chatservice::HandleFriendRequest* request,
                                     ::chatservice::HandleFriendResponse* response,
                                     ::google::protobuf::Closure* done) {
  if (request->handler_id() == 0 || request->request_id() == 0 ||
      (request->action() != 1 && request->action() != 2)) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  int rc = service::handleFriendRequest(request->handler_id(),
                                        request->request_id(), request->action());
  setResult(response->mutable_result(), rc);
  done->Run();
}

void SocialServiceImpl::ListFriends(::google::protobuf::RpcController* controller,
                                    const ::chatservice::ListFriendsRequest* request,
                                    ::chatservice::ListFriendsResponse* response,
                                    ::google::protobuf::Closure* done) {
  if (request->user_id() == 0) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  std::vector<model::FriendRow> rows;
  int rc = service::listFriends(request->user_id(), &rows);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    for (const auto& f : rows) {
      FriendItem* item = response->add_friends();
      item->set_friend_id(f.friend_id);
      item->set_remark(f.remark);
      item->set_apply_msg(f.apply_msg);
      item->set_created_at(f.confirm_at);  // FriendItem.created_at = 成为好友时间
    }
  }
  done->Run();
}

void SocialServiceImpl::DeleteFriend(::google::protobuf::RpcController* controller,
                                     const ::chatservice::DeleteFriendRequest* request,
                                     ::chatservice::DeleteFriendResponse* response,
                                     ::google::protobuf::Closure* done) {
  const uint64_t uid = request->user_id();
  const uint64_t fid = request->friend_id();
  if (uid == 0 || fid == 0) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (uid == fid) {
    setResult(response->mutable_result(), ErrCode::ERR_SELF_OP);
    done->Run();
    return;
  }
  int rc = service::deleteFriend(uid, fid);
  setResult(response->mutable_result(), rc);
  done->Run();
}

void SocialServiceImpl::AddBlacklist(::google::protobuf::RpcController* controller,
                                     const ::chatservice::AddBlacklistRequest* request,
                                     ::chatservice::AddBlacklistResponse* response,
                                     ::google::protobuf::Closure* done) {
  const uint64_t uid = request->user_id();
  const uint64_t blocked = request->blocked_user_id();
  if (uid == 0 || blocked == 0 || request->reason().size() > kMaxReason) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (uid == blocked) {
    setResult(response->mutable_result(), ErrCode::ERR_SELF_OP);
    done->Run();
    return;
  }
  int rc = service::addBlacklist(uid, blocked, request->reason());
  setResult(response->mutable_result(), rc);
  done->Run();
}

void SocialServiceImpl::ListBlacklist(::google::protobuf::RpcController* controller,
                                      const ::chatservice::ListBlacklistRequest* request,
                                      ::chatservice::ListBlacklistResponse* response,
                                      ::google::protobuf::Closure* done) {
  if (request->user_id() == 0) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  std::vector<model::BlackRow> rows;
  int rc = service::listBlacklist(request->user_id(), &rows);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    for (const auto& b : rows) {
      BlackItem* item = response->add_list();
      item->set_blocked_user_id(b.blocked_user_id);
      item->set_reason(b.reason);
      item->set_created_at(b.created_at);
    }
  }
  done->Run();
}

void SocialServiceImpl::RemoveBlacklist(::google::protobuf::RpcController* controller,
                                        const ::chatservice::RemoveBlacklistRequest* request,
                                        ::chatservice::RemoveBlacklistResponse* response,
                                        ::google::protobuf::Closure* done) {
  const uint64_t uid = request->user_id();
  const uint64_t blocked = request->blocked_user_id();
  if (uid == 0 || blocked == 0) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (uid == blocked) {
    setResult(response->mutable_result(), ErrCode::ERR_SELF_OP);
    done->Run();
    return;
  }
  int rc = service::removeBlacklist(uid, blocked);
  setResult(response->mutable_result(), rc);
  done->Run();
}

} // namespace chatservice
