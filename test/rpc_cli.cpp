#include"AsyncLogging.hpp"
#include<filesystem>
#include<unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cstring>
#include "user.pb.h"
#include "mprpcapplication.h"
#include "rpcprovider.h"
#include "zookeeperutil.h"
#include "rpcheader.pb.h"
namespace fs = std::filesystem;
wangt::AsyncLogging *g_asyncLog=nullptr;
void asyncwrite(const string& info){
    g_asyncLog->append(info);
}
void asyncflush(){
    g_asyncLog->flush();
}

class MprpcChannel:public ::google::protobuf::RpcChannel{
    void CallMethod(const ::google::protobuf::MethodDescriptor* method,
                  ::google::protobuf::RpcController* controller,
                  const ::google::protobuf::Message* request,
                  ::google::protobuf::Message* response,
                  ::google::protobuf::Closure* done){
         const google::protobuf::ServiceDescriptor *sd = method->service();
        std::string service_name = sd->name();    // service_name
        std::string method_name = method->name(); // method_name

        // 获取参数的序列化字符串长度 args_size
        uint32_t args_size = 0;
        std::string args_str;
        if (request->SerializeToString(&args_str))
        {
            args_size = args_str.size();
        }
        else
        {
            controller->SetFailed("serialize request error!");
            return;
        }

        // 定义rpc的请求header
        mprpc::RpcHeader rpcHeader;
        rpcHeader.set_service_name(service_name);
        rpcHeader.set_method_name(method_name);
        rpcHeader.set_args_size(args_size);

        uint32_t header_size = 0;
        std::string rpc_header_str;
        if (rpcHeader.SerializeToString(&rpc_header_str))
        {
            header_size = rpc_header_str.size();
        }
        else
        {
            controller->SetFailed("serialize rpc header error!");
            return;
        }

        // 组织待发送的rpc请求的字符串
        std::string send_rpc_str;
        send_rpc_str.insert(0, std::string((char *)&header_size, 4)); // header_size
        send_rpc_str += rpc_header_str;                               // rpcheader
        send_rpc_str += args_str;                                     // args

        // 打印调试信息
        std::cout << "============================================" << std::endl;
        std::cout << "header_size: " << header_size << std::endl;
        std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
        std::cout << "service_name: " << service_name << std::endl;
        std::cout << "method_name: " << method_name << std::endl;
        std::cout << "args_str: " << args_str << std::endl;
        std::cout << "============================================" << std::endl;

        // 使用tcp编程，完成rpc方法的远程调用
        int clientfd = socket(AF_INET, SOCK_STREAM, 0);
        if (-1 == clientfd)
        {
            char errtxt[512] = {0};
            sprintf(errtxt, "create socket error! errno:%d", errno);
            controller->SetFailed(errtxt);
            return;
        }

        // 给 recv 设置 10s 超时：即使服务器异常（不回包也不关连接）也不能无限阻塞
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // 读取配置文件rpcserver的信息
        // std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserverip");
        // uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());
        // rpc调用方想调用service_name的method_name服务，需要查询zk上该服务所在的host信息
        ZkClient zkCli;
        zkCli.Start();
        //  /UserServiceRpc/Login
        std::string method_path = "/" + service_name + "/" + method_name;
        // 127.0.0.1:8000
        std::string host_data = zkCli.GetData(method_path.c_str());
        if (host_data == "")
        {
            controller->SetFailed(method_path + " is not exist!");
            return;
        }
        int idx = host_data.find(":");
        if (idx == -1)
        {
            controller->SetFailed(method_path + " address is invalid!");
            return;
        }
        std::string ip = host_data.substr(0, idx);
        uint16_t port = atoi(host_data.substr(idx + 1, host_data.size() - idx).c_str());
        std::cout<<"ip:port = "<<ip<<":"<<port<<std::endl;
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

        // 连接rpc服务节点
        if (-1 == connect(clientfd, (struct sockaddr *)&server_addr, sizeof(server_addr)))
        {
            close(clientfd);
            char errtxt[512] = {0};
            sprintf(errtxt, "connect error! errno:%d", errno);
            controller->SetFailed(errtxt);
            return;
        }

        // 发送rpc请求
        if (-1 == send(clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0))
        {
            close(clientfd);
            char errtxt[512] = {0};
            sprintf(errtxt, "send error! errno:%d", errno);
            controller->SetFailed(errtxt);
            return;
        }

        // 接收rpc请求的响应值
        char recv_buf[1024] = {0};
        int recv_size = recv(clientfd, recv_buf, sizeof(recv_buf), 0);
        if (recv_size < 0)
        {
            close(clientfd);
            char errtxt[512] = {0};
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                sprintf(errtxt, "recv timeout! server no response");
            else
                sprintf(errtxt, "recv error! errno:%d", errno);
            controller->SetFailed(errtxt);
            return;
        }
        if (recv_size == 0)
        {
            // 服务器未返回数据就关闭连接：通常是服务端未部署该服务/方法，直接按失败处理
            close(clientfd);
            controller->SetFailed("server closed connection without response (method not deployed?)");
            return;
        }

        // 反序列化rpc调用的响应数据
        // std::string response_str(recv_buf, 0, recv_size); // bug出现问题，recv_buf中遇到\0后面的数据就存不下来了，导致反序列化失败
        // if (!response->ParseFromString(response_str))
        if (!response->ParseFromArray(recv_buf, recv_size))
        {
            close(clientfd);
            char errtxt[512] = {0};
            snprintf(errtxt, sizeof(errtxt), "parse response error! len:%d", recv_size);
            controller->SetFailed(errtxt);
            return;
        }

        close(clientfd);
    }
};

// main 里需要传一个真实的 controller 才能取到失败原因（原代码传 nullptr，出错会解引用崩溃）
class SimpleController : public ::google::protobuf::RpcController
{
public:
    void Reset() override { failed_ = false; errText_.clear(); }
    bool Failed() const override { return failed_; }
    std::string ErrorText() const override { return errText_; }
    void StartCancel() override {}
    void SetFailed(const std::string &reason) override { failed_ = true; errText_ = reason; }
    bool IsCanceled() const override { return false; }
    void NotifyOnCancel(::google::protobuf::Closure *callback) override {}

private:
    bool failed_ = false;
    std::string errText_;
};

int main(int argc , char** argv){
    //测试日志模块
    string path = "//home/wangt/chat/logmsg/cli/";
    if (!fs::exists(path))
    {
        fs::create_directories(path);
    }
    g_asyncLog = new wangt::AsyncLogging(path, 1024 * 1024 * 10);
    g_asyncLog->start();
    wangt::Logger::setOutput(asyncwrite);
    wangt::Logger::setFlush(asyncflush);

    MprpcApplication::Init(argc, argv);
    fixbug::UserServiceRpc_Stub stub(new MprpcChannel());
    fixbug::RegisterRequest request;
    request.set_name("wangt");
    request.set_pwd("20050610");
    request.set_id(1);
    fixbug::RegisterResponse response;
    SimpleController controller;
    stub.Resgister(&controller, &request, &response, nullptr);
    if (controller.Failed())
    {
        std::cout << "rpc login failed: " << controller.ErrorText() << std::endl;
        return 1;
    }
     if (0 == response.result().errcode())
    {
        std::cout << "rpc login response success:" << response.sucess() << std::endl;
    }
    else
    {
        std::cout << "rpc login response error : " << response.result().errmsg() << std::endl;
    }
    std::cout<<"message:"<<response.result().errmsg()<<std::endl;

    return 0;
}