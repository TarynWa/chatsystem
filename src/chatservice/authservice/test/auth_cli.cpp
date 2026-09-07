// AuthService 测试客户端(S0 E2E): 注册→自动登录→verify→登出 → 再登录→verify→登出
//   → 重复注册(1101) / 错密码(1103) / 空设备(1001) 负向分支
// 仿 test/rpc_cli.cpp 的裸 socket 通路; 本客户端直连 conf 里的 rpcserverip/port(免 zk)。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "auth.pb.h"
#include "mprpcapplication.h"
#include "mprpcconfig.h"
#include "rpcheader.pb.h"

namespace {

// 复用 rpc_cli.cpp 的短连接通道逻辑, 但目标地址从 conf 直读(不走 zk)
class MprpcChannel : public ::google::protobuf::RpcChannel {
 public:
  void CallMethod(const ::google::protobuf::MethodDescriptor* method,
                  ::google::protobuf::RpcController* controller,
                  const ::google::protobuf::Message* request,
                  ::google::protobuf::Message* response,
                  ::google::protobuf::Closure* done) override {
    std::string service_name = method->service()->name();
    std::string method_name = method->name();

    std::string args_str;
    if (!request->SerializeToString(&args_str)) {
      controller->SetFailed("serialize request error!");
      return;
    }
    mprpc::RpcHeader rpcHeader;
    rpcHeader.set_service_name(service_name);
    rpcHeader.set_method_name(method_name);
    rpcHeader.set_args_size((uint32_t)args_str.size());
    std::string rpc_header_str;
    if (!rpcHeader.SerializeToString(&rpc_header_str)) {
      controller->SetFailed("serialize rpc header error!");
      return;
    }
    uint32_t header_size = (uint32_t)rpc_header_str.size();
    std::string send_rpc_str;
    send_rpc_str.append((const char*)&header_size, 4);
    send_rpc_str += rpc_header_str;
    send_rpc_str += args_str;

    MprpcConfig& cfg = MprpcApplication::GetConfig();
    std::string ip = cfg.Load("rpcserverip");
    if (ip.empty()) ip = "127.0.0.1";
    int port = atoi(cfg.Load("rpcserverport").c_str());
    if (port <= 0) port = 8001;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      controller->SetFailed("create socket error");
      return;
    }
    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
      close(fd);
      controller->SetFailed("connect error");
      return;
    }
    if (send(fd, send_rpc_str.data(), send_rpc_str.size(), 0) < 0) {
      close(fd);
      controller->SetFailed("send error");
      return;
    }
    char buf[8192];
    int n = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n < 0) {
      controller->SetFailed("recv timeout / error");
      return;
    }
    if (n == 0 || !response->ParseFromArray(buf, n)) {
      controller->SetFailed("parse response error");
      return;
    }
  }
};

class SimpleController : public ::google::protobuf::RpcController {
 public:
  void Reset() override { failed_ = false; err_.clear(); }
  bool Failed() const override { return failed_; }
  std::string ErrorText() const override { return err_; }
  void StartCancel() override {}
  void SetFailed(const std::string& reason) override { failed_ = true; err_ = reason; }
  bool IsCanceled() const override { return false; }
  void NotifyOnCancel(::google::protobuf::Closure*) override {}

 private:
  bool failed_ = false;
  std::string err_;
};

chatservice::DeviceInfo mkDevice(const std::string& did) {
  chatservice::DeviceInfo d;
  d.set_device_id(did);
  d.set_device_name("auth_cli");
  d.set_device_type(4);  // PC
  d.set_os_version("test");
  d.set_app_version("0.1");
  return d;
}

} // namespace

int main(int argc, char** argv) {
  MprpcApplication::Init(argc, argv);
  chatservice::AuthServiceRpc_Stub stub(new MprpcChannel());
  SimpleController ctrl;

  int pass = 0, fail = 0;
  auto check = [&](const char* name, bool ok, const std::string& extra) {
    if (ok) {
      ++pass;
      printf("  [PASS] %s\n", name);
    } else {
      ++fail;
      printf("  [FAIL] %s  %s\n", name, extra.c_str());
    }
  };

  // 每次运行用带时间戳的用户名, 幂等可重跑
  char suffix[32];
  snprintf(suffix, sizeof(suffix), "%ld", (long)time(nullptr) % 1000000000L);
  std::string username = "u" + std::string(suffix);
  std::string password = "Passw0rd_1";
  std::string did1 = "cli-dev-" + std::string(suffix);
  std::string did2 = "cli-dev2-" + std::string(suffix);

  std::string access1, refresh1;
  uint64_t uid = 0;

  // ---------- 1. 注册 → 自动登录下发 token ----------
  {
    chatservice::RegisterRequest req;
    req.set_username(username);
    req.set_password(password);
    req.set_nickname(username);
    *req.mutable_device() = mkDevice(did1);
    chatservice::RegisterResponse rsp;
    ctrl.Reset();
    stub.Register(&ctrl, &req, &rsp, nullptr);
    bool ok = !ctrl.Failed() && rsp.result().code() == 0 && rsp.user_id() != 0 &&
              !rsp.tokens().access_token().empty();
    check("Register + auto-login(拿 token)", ok,
          ctrl.Failed() ? ctrl.ErrorText() : ("code=" + std::to_string(rsp.result().code())));
    if (ok) {
      uid = rsp.user_id();
      access1 = rsp.tokens().access_token();
      refresh1 = rsp.tokens().refresh_token();
    }
  }

  // ---------- 2. 凭 access_token 校验 ----------
  if (!access1.empty()) {
    chatservice::VerifyTokenRequest vq;
    vq.set_token(access1);
    chatservice::VerifyTokenResponse vr;
    ctrl.Reset();
    stub.VerifyToken(&ctrl, &vq, &vr, nullptr);
    check("VerifyToken(注册自动登录的 access)", !ctrl.Failed() && vr.valid() && vr.user_id() == uid,
          vr.result().msg());
  }

  // ---------- 3. 登出后原 token 失效 ----------
  if (!access1.empty()) {
    chatservice::LogoutRequest lq;
    lq.set_token(access1);
    chatservice::LogoutResponse lr;
    ctrl.Reset();
    stub.Logout(&ctrl, &lq, &lr, nullptr);
    check("Logout(access)", !ctrl.Failed() && lr.result().code() == 0, lr.result().msg());

    chatservice::VerifyTokenRequest vq;
    vq.set_token(access1);
    chatservice::VerifyTokenResponse vr;
    ctrl.Reset();
    stub.VerifyToken(&ctrl, &vq, &vr, nullptr);
    check("登出后 token 失效", !ctrl.Failed() && !vr.valid(), vr.result().msg());
  }

  // ---------- 4. 重新登录(第二台设备) ----------
  std::string access2;
  {
    chatservice::LoginRequest req;
    req.set_account(username);  // username 形态
    req.set_password(password);
    *req.mutable_device() = mkDevice(did2);
    req.set_ip("127.0.0.1");
    req.set_user_agent("auth_cli");
    chatservice::LoginResponse rsp;
    ctrl.Reset();
    stub.Login(&ctrl, &req, &rsp, nullptr);
    bool ok = !ctrl.Failed() && rsp.result().code() == 0 &&
              rsp.user().username() == username && !rsp.tokens().access_token().empty();
    check("Login(username)", ok, ctrl.Failed() ? ctrl.ErrorText() : rsp.result().msg());
    if (ok) access2 = rsp.tokens().access_token();
  }

  // ---------- 5. 用 email 形态登录(注册时 email 为空, 应 1102) ----------
  {
    chatservice::LoginRequest req;
    req.set_account("nobody@example.com");
    req.set_password(password);
    *req.mutable_device() = mkDevice(did2);
    chatservice::LoginResponse rsp;
    ctrl.Reset();
    stub.Login(&ctrl, &req, &rsp, nullptr);
    check("Login(email 不存在 → 1102)", !ctrl.Failed() && rsp.result().code() == 1102,
          std::to_string(rsp.result().code()));
  }

  // ---------- 6. 错密码 → 1103 ----------
  {
    chatservice::LoginRequest req;
    req.set_account(username);
    req.set_password(password + "x");
    *req.mutable_device() = mkDevice(did2);
    chatservice::LoginResponse rsp;
    ctrl.Reset();
    stub.Login(&ctrl, &req, &rsp, nullptr);
    bool locked = rsp.result().code() == 1104;  // 连续错5次后会是锁
    bool wrong = rsp.result().code() == 1103;
    check("Login(错密码 → 1103)", !ctrl.Failed() && (wrong || locked),
          "code=" + std::to_string(rsp.result().code()));
  }

  // ---------- 7. 重复注册 → 1101 ----------
  {
    chatservice::RegisterRequest req;
    req.set_username(username);
    req.set_password(password);
    *req.mutable_device() = mkDevice(did1);
    chatservice::RegisterResponse rsp;
    ctrl.Reset();
    stub.Register(&ctrl, &req, &rsp, nullptr);
    check("重复注册 → 1101", !ctrl.Failed() && rsp.result().code() == 1101,
          std::to_string(rsp.result().code()));
  }

  // ---------- 8. 空设备登录 → 1001 ----------
  {
    chatservice::LoginRequest req;
    req.set_account(username);
    req.set_password(password);
    chatservice::LoginResponse rsp;
    ctrl.Reset();
    stub.Login(&ctrl, &req, &rsp, nullptr);
    check("Login(空设备 → 1001)", !ctrl.Failed() && rsp.result().code() == 1001,
          std::to_string(rsp.result().code()));
  }

  printf("\n===== auth_cli done: pass=%d fail=%d =====\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
