// MsgService(S0 单聊)测试客户端: 双用户收发 + 拉新(离线收件箱) + 历史 + 负向分支
//   send(A→B) → pull(B)得1条/再pull空 → pull(A)得1条/再pull空 → history 双向可见
//   → 负向: 自聊1401 / 空文本1001 / 非法类型1001
// 直连 conf 的 rpcserverip/port(免 zk); S0 信任调用方 uid → 固定测试 uid。
// 幂等说明: 重复运行前先清 im_message(见 README 验收)。
#include <cstdint>
#include <cstdio>
#include <string>

#include "base/rpc_client.h"
#include "msg.pb.h"

using chatservice::MsgServiceRpc_Stub;
using chatservice::rpc::DirectChannel;
using chatservice::rpc::SimpleController;

namespace {
constexpr uint64_t UID_A = 1000001ULL;  // 测试用(非 im_auth 用户, S0 信任 uid)
constexpr uint64_t UID_B = 1000002ULL;
} // namespace

int main(int argc, char** argv) {
  MprpcApplication::Init(argc, argv);
  DirectChannel ch;
  MsgServiceRpc_Stub stub(&ch);

  int pass = 0, fail = 0;
  auto check = [&](const char* name, bool ok, const std::string& extra) {
    if (ok) {
      ++pass;
      printf("  [PASS] %s\n", name);
    } else {
      ++fail;
      printf("  [FAIL] %s  (%s)\n", name, extra.c_str());
    }
  };

  // ---- 收发 ----
  chatservice::SendMsgRequest sendA;
  sendA.set_from_user(UID_A);
  sendA.set_to_user(UID_B);
  sendA.set_msg_type(0);
  sendA.set_content("hello from A");
  chatservice::SendMsgResponse sendRsp;
  {
    SimpleController c;
    stub.SendMsg(&c, &sendA, &sendRsp, nullptr);
    check("send A->B 文本", !c.Failed() && sendRsp.result().code() == 0 &&
                                sendRsp.msg_id() > 0,
          "code=" + std::to_string(sendRsp.result().code()));
  }
  const uint64_t firstMid = sendRsp.msg_id();

  chatservice::SendMsgRequest sendB;
  sendB.set_from_user(UID_B);
  sendB.set_to_user(UID_A);
  sendB.set_msg_type(0);
  sendB.set_content("hi back from B");
  chatservice::SendMsgResponse sendRsp2;
  {
    SimpleController c;
    stub.SendMsg(&c, &sendB, &sendRsp2, nullptr);
    check("send B->A 文本", !c.Failed() && sendRsp2.result().code() == 0 &&
                                sendRsp2.msg_id() > sendRsp.msg_id(),
          "code=" + std::to_string(sendRsp2.result().code()));
  }

  // ---- 拉新(offline 收件箱) ----
  chatservice::PullNewMsgRequest pull;
  pull.set_user_id(UID_B);
  chatservice::PullNewMsgResponse pullRsp;
  {
    SimpleController c;
    stub.PullNewMsg(&c, &pull, &pullRsp, nullptr);
    bool gotHello = pullRsp.list_size() == 1 && pullRsp.list(0).from_user() == UID_A &&
                    pullRsp.list(0).msg_id() == firstMid &&
                    pullRsp.list(0).content() == "hello from A";
    check("pull(B) 拉到 A 的消息", !c.Failed() && pullRsp.result().code() == 0 && gotHello,
          "n=" + std::to_string(pullRsp.list_size()));
  }
  {
    SimpleController c;
    stub.PullNewMsg(&c, &pull, &pullRsp, nullptr);
    check("pull(B) 再次为空(已置 status=1)", !c.Failed() && pullRsp.result().code() == 0 &&
                                                 pullRsp.list_size() == 0,
          "n=" + std::to_string(pullRsp.list_size()));
  }
  pull.set_user_id(UID_A);
  {
    SimpleController c;
    stub.PullNewMsg(&c, &pull, &pullRsp, nullptr);
    bool gotBack = pullRsp.list_size() == 1 && pullRsp.list(0).from_user() == UID_B &&
                   pullRsp.list(0).content() == "hi back from B";
    check("pull(A) 拉到 B 的消息", !c.Failed() && pullRsp.result().code() == 0 && gotBack,
          "n=" + std::to_string(pullRsp.list_size()));
  }
  {
    SimpleController c;
    stub.PullNewMsg(&c, &pull, &pullRsp, nullptr);
    check("pull(A) 再次为空", !c.Failed() && pullRsp.result().code() == 0 &&
                                  pullRsp.list_size() == 0,
          "n=" + std::to_string(pullRsp.list_size()));
  }

  // ---- 历史(双向, msg_id 倒序) ----
  chatservice::GetHistoryRequest gh;
  gh.set_user_id(UID_A);
  gh.set_peer_id(UID_B);
  chatservice::GetHistoryResponse ghRsp;
  {
    SimpleController c;
    stub.GetHistory(&c, &gh, &ghRsp, nullptr);
    bool ok = ghRsp.result().code() == 0 && ghRsp.list_size() == 2 &&
              !ghRsp.has_more();
    if (ok) {  // 最新在前: B→A("hi back") 应排第一
      ok = ghRsp.list(0).from_user() == UID_B && ghRsp.list(1).from_user() == UID_A;
    }
    check("history(A,B) 双向2条且倒序", ok,
          "n=" + std::to_string(ghRsp.list_size()));
  }
  gh.set_user_id(UID_B);
  gh.set_peer_id(UID_A);
  {
    SimpleController c;
    stub.GetHistory(&c, &gh, &ghRsp, nullptr);
    check("history(B,A) 同2条", !c.Failed() && ghRsp.result().code() == 0 &&
                                    ghRsp.list_size() == 2,
          "n=" + std::to_string(ghRsp.list_size()));
  }
  // 分页1: before_id=最新那条 → 应只返回 1 条更早(历史分页的下翻, 曾因 AND/OR 优先级被绕过)
  gh.set_user_id(UID_A);
  gh.set_peer_id(UID_B);
  gh.set_before_id(ghRsp.list(0).msg_id());  // ghRsp 仍是 (B,A) 响应, list(0)=最新 hiBack
  {
    SimpleController c;
    stub.GetHistory(&c, &gh, &ghRsp, nullptr);
    bool ok = !c.Failed() && ghRsp.result().code() == 0 && ghRsp.list_size() == 1 &&
              ghRsp.list(0).msg_id() == firstMid && !ghRsp.has_more();
    check("history 翻页: before=最新 → 剩1条更早", ok,
          "n=" + std::to_string(ghRsp.list_size()));
  }
  // 分页2: before_id=最早那条 msg_id → 返回空(无更早)
  gh.set_before_id(ghRsp.list(0).msg_id());  // 上一步只剩 hello, 即最早一条
  {
    SimpleController c;
    stub.GetHistory(&c, &gh, &ghRsp, nullptr);
    check("history 翻页到头为空", !c.Failed() && ghRsp.result().code() == 0 &&
                                      ghRsp.list_size() == 0 && !ghRsp.has_more(),
          "n=" + std::to_string(ghRsp.list_size()));
  }

  // ---- 负向 ----
  chatservice::SendMsgRequest bad;
  bad.set_from_user(UID_A);
  bad.set_to_user(UID_A);
  bad.set_msg_type(0);
  bad.set_content("self");
  {
    SimpleController c;
    stub.SendMsg(&c, &bad, &sendRsp, nullptr);
    check("自聊 → 1401", !c.Failed() && sendRsp.result().code() == 1401,
          "code=" + std::to_string(sendRsp.result().code()));
  }
  bad.set_to_user(UID_B);
  bad.set_content("");
  {
    SimpleController c;
    stub.SendMsg(&c, &bad, &sendRsp, nullptr);
    check("空文本 → 1001", !c.Failed() && sendRsp.result().code() == 1001,
          "code=" + std::to_string(sendRsp.result().code()));
  }
  bad.set_content("x");
  bad.set_msg_type(99);
  {
    SimpleController c;
    stub.SendMsg(&c, &bad, &sendRsp, nullptr);
    check("非法消息类型 → 1001", !c.Failed() && sendRsp.result().code() == 1001,
          "code=" + std::to_string(sendRsp.result().code()));
  }

  printf("\n===== msg_cli done: pass=%d fail=%d =====\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
