// header-only 直连 mprpc 短连接客户端(S0 测试 CLI 复用, 免去各 cli 内联复制 channel)。
// 目标地址默认从 MprpcApplication conf 读 rpcserverip/rpcserverport(直连, 不走 zk);
// 语义同 authservice/test/auth_cli.cpp 的 MprpcChannel(10s recv 超时, 一发一收即断)。
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>

#include "mprpcapplication.h"
#include "mprpcconfig.h"
#include "rpcheader.pb.h"

namespace chatservice::rpc {

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

// 直连通道: 每调用一次新建 socket; 单次 recv 到 8KB 缓冲并反序列化
class DirectChannel : public ::google::protobuf::RpcChannel {
 public:
  DirectChannel() {
    MprpcConfig& cfg = MprpcApplication::GetConfig();
    ip_ = cfg.Load("rpcserverip");
    if (ip_.empty()) ip_ = "127.0.0.1";
    port_ = atoi(cfg.Load("rpcserverport").c_str());
    if (port_ <= 0) port_ = 8001;
  }

  void CallMethod(const ::google::protobuf::MethodDescriptor* method,
                  ::google::protobuf::RpcController* controller,
                  const ::google::protobuf::Message* request,
                  ::google::protobuf::Message* response,
                  ::google::protobuf::Closure* done) override {
    std::string args_str;
    if (!request->SerializeToString(&args_str)) {
      controller->SetFailed("serialize request error!");
      return;
    }
    mprpc::RpcHeader rpcHeader;
    rpcHeader.set_service_name(method->service()->name());
    rpcHeader.set_method_name(method->name());
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

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      controller->SetFailed("create socket error");
      return;
    }
    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = inet_addr(ip_.c_str());
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

 private:
  std::string ip_;
  int port_ = 0;
};

} // namespace chatservice::rpc
