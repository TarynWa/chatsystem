// SocialService(S0 好友+黑名单)测试客户端:
//   加好友申请→重复1502→越权1505→接受→双向列表互见→已是好友1501→重复处理1503
//   →黑名单 加/幂等/列表→对方拉黑你1507→你已拉黑对方1508→移除→空
//   →删除好友→重加+重接受(从 status=3 复活)→结束清理(删除好友+移除黑名单, 便于重复运行)
// 直连 conf 的 rpcserverip/port(免 zk); S0 信任调用方 uid → 固定测试 uid。
// 幂等说明: 用例结束时已清理 A-B 好友与 A-C 黑名单, 可重复运行; 中途中断可用
//   `mysql -uroot im_social -e "TRUNCATE friend_requests; TRUNCATE friends; TRUNCATE blacklist;"`
//   重置(见 README 验收)。
#include <cstdint>
#include <cstdio>
#include <string>

#include "base/rpc_client.h"
#include "social.pb.h"

using chatservice::SocialServiceRpc_Stub;
using chatservice::rpc::DirectChannel;
using chatservice::rpc::SimpleController;

namespace {
constexpr uint64_t UID_A = 1000001ULL;  // 测试用(非 im_auth 用户, S0 信任 uid)
constexpr uint64_t UID_B = 1000002ULL;
constexpr uint64_t UID_C = 1000003ULL;
} // namespace

int main(int argc, char** argv) {
  MprpcApplication::Init(argc, argv);
  DirectChannel ch;
  SocialServiceRpc_Stub stub(&ch);

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
  auto codeOf = [](const chatservice::Result& r) { return std::to_string(r.code()); };

  // ---------- 加好友 A→B ----------
  chatservice::AddFriendRequest af;
  af.set_from_user(UID_A);
  af.set_to_user(UID_B);
  af.set_apply_msg("hi A");
  chatservice::AddFriendResponse afRsp;
  {
    SimpleController c;
    stub.AddFriend(&c, &af, &afRsp, nullptr);
    check("加好友 A→B 成功", !c.Failed() && afRsp.result().code() == 0 &&
                                 afRsp.request_id() > 0 && !afRsp.expire_at().empty(),
          "code=" + codeOf(afRsp.result()));
  }
  const uint64_t reqId1 = afRsp.request_id();

  {
    SimpleController c;
    stub.AddFriend(&c, &af, &afRsp, nullptr);
    check("重复申请 → 1502", !c.Failed() && afRsp.result().code() == 1502,
          "code=" + codeOf(afRsp.result()));
  }

  chatservice::AddFriendRequest self;
  self.set_from_user(UID_A);
  self.set_to_user(UID_A);
  self.set_apply_msg("x");
  {
    SimpleController c;
    stub.AddFriend(&c, &self, &afRsp, nullptr);
    check("自己加自己 → 1506", !c.Failed() && afRsp.result().code() == 1506,
          "code=" + codeOf(afRsp.result()));
  }

  // ---------- 处理申请 ----------
  chatservice::HandleFriendRequest hf;
  chatservice::HandleFriendResponse hfRsp;
  hf.set_request_id(reqId1);
  hf.set_action(1);
  hf.set_handler_id(UID_C);
  {
    SimpleController c;
    stub.HandleFriend(&c, &hf, &hfRsp, nullptr);
    check("非接收人处理 → 1505", !c.Failed() && hfRsp.result().code() == 1505,
          "code=" + codeOf(hfRsp.result()));
  }
  hf.set_handler_id(UID_B);
  {
    SimpleController c;
    stub.HandleFriend(&c, &hf, &hfRsp, nullptr);
    check("B 接受申请成功", !c.Failed() && hfRsp.result().code() == 0,
          "code=" + codeOf(hfRsp.result()));
  }

  // ---------- 好友列表(双向互见) ----------
  chatservice::ListFriendsRequest lf;
  chatservice::ListFriendsResponse lfRsp;
  lf.set_user_id(UID_A);
  {
    SimpleController c;
    stub.ListFriends(&c, &lf, &lfRsp, nullptr);
    bool ok = lfRsp.result().code() == 0 && lfRsp.friends_size() == 1 &&
              lfRsp.friends(0).friend_id() == UID_B &&
              lfRsp.friends(0).apply_msg() == "hi A";
    check("A 好友列表含 B(带申请语)", ok,
          "n=" + std::to_string(lfRsp.friends_size()) +
              " code=" + codeOf(lfRsp.result()));
  }
  lf.set_user_id(UID_B);
  {
    SimpleController c;
    stub.ListFriends(&c, &lf, &lfRsp, nullptr);
    check("B 好友列表含 A(双向一致)", lfRsp.result().code() == 0 &&
                                         lfRsp.friends_size() == 1 &&
                                         lfRsp.friends(0).friend_id() == UID_A,
          "n=" + std::to_string(lfRsp.friends_size()));
  }

  {
    SimpleController c;
    stub.AddFriend(&c, &af, &afRsp, nullptr);
    check("已是好友再申请 → 1501", !c.Failed() && afRsp.result().code() == 1501,
          "code=" + codeOf(afRsp.result()));
  }
  {
    SimpleController c;
    stub.HandleFriend(&c, &hf, &hfRsp, nullptr);
    check("重复处理已接受 → 1503", !c.Failed() && hfRsp.result().code() == 1503,
          "code=" + codeOf(hfRsp.result()));
  }
  hf.set_request_id(99999999ULL);
  {
    SimpleController c;
    stub.HandleFriend(&c, &hf, &hfRsp, nullptr);
    check("处理不存在的申请 → 1503", !c.Failed() && hfRsp.result().code() == 1503,
          "code=" + codeOf(hfRsp.result()));
  }

  // ---------- 黑名单 A 拉黑 C ----------
  chatservice::AddBlacklistRequest ab;
  ab.set_user_id(UID_A);
  ab.set_blocked_user_id(UID_C);
  ab.set_reason("spam");
  chatservice::AddBlacklistResponse abRsp;
  {
    SimpleController c;
    stub.AddBlacklist(&c, &ab, &abRsp, nullptr);
    check("拉黑 C 成功", !c.Failed() && abRsp.result().code() == 0,
          "code=" + codeOf(abRsp.result()));
  }
  {
    SimpleController c;
    stub.AddBlacklist(&c, &ab, &abRsp, nullptr);
    check("重复拉黑幂等", !c.Failed() && abRsp.result().code() == 0,
          "code=" + codeOf(abRsp.result()));
  }

  chatservice::ListBlacklistRequest lb;
  chatservice::ListBlacklistResponse lbRsp;
  lb.set_user_id(UID_A);
  {
    SimpleController c;
    stub.ListBlacklist(&c, &lb, &lbRsp, nullptr);
    check("A 黑名单含 C", lbRsp.result().code() == 0 && lbRsp.list_size() == 1 &&
                             lbRsp.list(0).blocked_user_id() == UID_C,
          "n=" + std::to_string(lbRsp.list_size()));
  }

  chatservice::AddFriendRequest fromC;
  fromC.set_from_user(UID_C);
  fromC.set_to_user(UID_A);
  fromC.set_apply_msg("c to a");
  {
    SimpleController c;
    stub.AddFriend(&c, &fromC, &afRsp, nullptr);
    check("C 申请 A(被拉黑) → 1507", !c.Failed() && afRsp.result().code() == 1507,
          "code=" + codeOf(afRsp.result()));
  }
  chatservice::AddFriendRequest aToC;
  aToC.set_from_user(UID_A);
  aToC.set_to_user(UID_C);
  aToC.set_apply_msg("a to c");
  {
    SimpleController c;
    stub.AddFriend(&c, &aToC, &afRsp, nullptr);
    check("A 申请已拉黑的 C → 1508", !c.Failed() && afRsp.result().code() == 1508,
          "code=" + codeOf(afRsp.result()));
  }

  chatservice::AddBlacklistRequest selfBlk;
  selfBlk.set_user_id(UID_A);
  selfBlk.set_blocked_user_id(UID_A);
  selfBlk.set_reason("x");
  {
    SimpleController c;
    stub.AddBlacklist(&c, &selfBlk, &abRsp, nullptr);
    check("拉黑自己 → 1506", !c.Failed() && abRsp.result().code() == 1506,
          "code=" + codeOf(abRsp.result()));
  }

  chatservice::RemoveBlacklistRequest rb;
  rb.set_user_id(UID_A);
  rb.set_blocked_user_id(UID_C);
  chatservice::RemoveBlacklistResponse rbRsp;
  {
    SimpleController c;
    stub.RemoveBlacklist(&c, &rb, &rbRsp, nullptr);
    check("移除黑名单 C", !c.Failed() && rbRsp.result().code() == 0,
          "code=" + codeOf(rbRsp.result()));
  }
  {
    SimpleController c;
    stub.ListBlacklist(&c, &lb, &lbRsp, nullptr);
    check("移除后黑名单为空", lbRsp.result().code() == 0 && lbRsp.list_size() == 0,
          "n=" + std::to_string(lbRsp.list_size()));
  }

  // ---------- 删除好友 + 重加复活 ----------
  chatservice::DeleteFriendRequest df;
  df.set_user_id(UID_A);
  df.set_friend_id(UID_B);
  chatservice::DeleteFriendResponse dfRsp;
  {
    SimpleController c;
    stub.DeleteFriend(&c, &df, &dfRsp, nullptr);
    check("删除好友 A→B", !c.Failed() && dfRsp.result().code() == 0,
          "code=" + codeOf(dfRsp.result()));
  }
  lf.set_user_id(UID_A);
  {
    SimpleController c;
    stub.ListFriends(&c, &lf, &lfRsp, nullptr);
    check("删除后 A 好友为空", lfRsp.result().code() == 0 && lfRsp.friends_size() == 0,
          "n=" + std::to_string(lfRsp.friends_size()));
  }
  lf.set_user_id(UID_B);
  {
    SimpleController c;
    stub.ListFriends(&c, &lf, &lfRsp, nullptr);
    check("删除后 B 好友为空(两侧一致)", lfRsp.result().code() == 0 &&
                                             lfRsp.friends_size() == 0,
          "n=" + std::to_string(lfRsp.friends_size()));
  }

  // 重加(旧行 status=3 → upsert 复活为 status=1)
  {
    SimpleController c;
    stub.AddFriend(&c, &af, &afRsp, nullptr);
    check("删除后可重加申请", !c.Failed() && afRsp.result().code() == 0 &&
                                 afRsp.request_id() > 0,
          "code=" + codeOf(afRsp.result()));
  }
  const uint64_t reqId2 = afRsp.request_id();
  hf.set_request_id(reqId2);
  hf.set_handler_id(UID_B);
  hf.set_action(1);
  {
    SimpleController c;
    stub.HandleFriend(&c, &hf, &hfRsp, nullptr);
    check("重接受成功(旧关系复活)", !c.Failed() && hfRsp.result().code() == 0,
          "code=" + codeOf(hfRsp.result()));
  }
  lf.set_user_id(UID_A);
  {
    SimpleController c;
    stub.ListFriends(&c, &lf, &lfRsp, nullptr);
    check("复活后 A 好友恢复含 B", lfRsp.result().code() == 0 &&
                                       lfRsp.friends_size() == 1 &&
                                       lfRsp.friends(0).friend_id() == UID_B,
          "n=" + std::to_string(lfRsp.friends_size()));
  }

  // ---------- 结束清理(便于重复运行) ----------
  {
    SimpleController c;
    stub.DeleteFriend(&c, &df, &dfRsp, nullptr);
    check("结束清理: 删好友", !c.Failed() && dfRsp.result().code() == 0,
          "code=" + codeOf(dfRsp.result()));
  }

  printf("\n===== social_cli done: pass=%d fail=%d =====\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
