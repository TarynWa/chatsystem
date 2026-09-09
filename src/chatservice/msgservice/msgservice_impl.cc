#include "msgservice/msgservice_impl.h"

#include "common/errcode.h"
#include "common/validate.h"
#include "msgservice/model/message.h"
#include "msgservice/service/message_service.h"

namespace chatservice {

namespace {
// S0 长度上限(messages.content 为 TEXT; 客户端过大内容直接拒绝)
constexpr size_t kMaxTextBytes = 5000;
constexpr size_t kMaxMediaBytes = 8192;
} // namespace

void MsgServiceImpl::setResult(Result* r, int code) {
  r->set_code(code);
  r->set_msg(ErrText(code));
}

void MsgServiceImpl::SendMsg(::google::protobuf::RpcController* controller,
                             const ::chatservice::SendMsgRequest* request,
                             ::chatservice::SendMsgResponse* response,
                             ::google::protobuf::Closure* done) {
  const uint64_t from = request->from_user();
  const uint64_t to = request->to_user();
  const int type = request->msg_type();
  const std::string& content = request->content();
  const std::string& media = request->media_urls();

  if (from == 0 || to == 0) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if (from == to) {
    setResult(response->mutable_result(), ErrCode::ERR_MSG_SELF);
    done->Run();
    return;
  }
  if (type < 0 || type > 6) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  if ((type == 0 && content.empty()) || content.size() > kMaxTextBytes ||
      media.size() > kMaxMediaBytes) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }

  model::SentMessage sent;
  int rc = service::sendMessage(from, to, type, content, media, &sent);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    response->set_msg_id(sent.msg_id);
    response->set_created_at(sent.created_at);
  }
  done->Run();
}

void MsgServiceImpl::PullNewMsg(::google::protobuf::RpcController* controller,
                                const ::chatservice::PullNewMsgRequest* request,
                                ::chatservice::PullNewMsgResponse* response,
                                ::google::protobuf::Closure* done) {
  const uint64_t uid = request->user_id();
  if (uid == 0) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  std::vector<model::MessageRow> rows;
  int rc = service::pullMessages(uid, request->limit(), &rows);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    for (const auto& m : rows) {
      MsgRecord* rec = response->add_list();
      rec->set_msg_id(m.msg_id);
      rec->set_from_user(m.from_user);
      rec->set_to_user(m.to_user);
      rec->set_msg_type(m.msg_type);
      rec->set_content(m.content);
      rec->set_media_urls(m.media_urls);
      rec->set_created_at(m.created_at);
    }
  }
  done->Run();
}

void MsgServiceImpl::GetHistory(::google::protobuf::RpcController* controller,
                                const ::chatservice::GetHistoryRequest* request,
                                ::chatservice::GetHistoryResponse* response,
                                ::google::protobuf::Closure* done) {
  const uint64_t me = request->user_id();
  const uint64_t peer = request->peer_id();
  if (me == 0 || peer == 0 || me == peer) {
    setResult(response->mutable_result(), ErrCode::ERR_PARAM);
    done->Run();
    return;
  }
  std::vector<model::MessageRow> rows;
  bool has_more = false;
  int rc = service::getHistory(me, peer, request->before_id(), request->limit(),
                               &rows, &has_more);
  setResult(response->mutable_result(), rc);
  if (rc == ErrCode::OK) {
    response->set_has_more(has_more);
    for (const auto& m : rows) {
      MsgRecord* rec = response->add_list();
      rec->set_msg_id(m.msg_id);
      rec->set_from_user(m.from_user);
      rec->set_to_user(m.to_user);
      rec->set_msg_type(m.msg_type);
      rec->set_content(m.content);
      rec->set_media_urls(m.media_urls);
      rec->set_created_at(m.created_at);
    }
  }
  done->Run();
}

} // namespace chatservice
