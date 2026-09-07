#include "AsyncLogging.hpp"
#include <filesystem>
#include <unistd.h>
#include <cstring>
#include "user.pb.h"
#include "mprpcapplication.h"
#include "rpcprovider.h"
namespace fs = std::filesystem;
wangt::AsyncLogging *g_asyncLog = nullptr;
void asyncwrite(const string &info)
{
    g_asyncLog->append(info);
}
void asyncflush()
{
    g_asyncLog->flush();
}
void newlog()
{
    string path = "//home/wangt/chat/logmsg/test1/";
    if (!fs::exists(path))
    {
        fs::create_directories(path);
    }
    g_asyncLog = new wangt::AsyncLogging(path, 1024 * 1024 * 10);
    g_asyncLog->start();
    wangt::Logger::setOutput(asyncwrite);
    wangt::Logger::setFlush(asyncflush);
}
class UserService : public fixbug::UserServiceRpc
{
    bool Login(string name, string pwd)
    {
        WT_LOG_INFO << "doing local service: Login";
        WT_LOG_INFO<< "name: "<<name<<" pwd: "<<pwd;
        return true;
    }
    bool Resgister(uint32_t id, string name, string pwd)
    {
        WT_LOG_INFO << "doing local service: Resgister";
        WT_LOG_INFO<< "id: "<<id<<" name: "<<name<<" pwd: "<<pwd;
        return true;
    }
    void Login(::google::protobuf::RpcController *controller,
               const ::fixbug::LoginRequest *request,
               ::fixbug::LoginResponse *response,
               ::google::protobuf::Closure *done)
    {
        string name = request->name();
        string pwd = request->pwd();
        bool login_result = Login(name, pwd);
        fixbug::ResultCode *code = response->mutable_result();
        code->set_errcode(login_result ? 0 : 1);
        code->set_errmsg(login_result ? "login success!" : "login error!");
        response->set_sucess(login_result);
        done->Run();
    }
    void Resgister(::google::protobuf::RpcController *controller,
                   const ::fixbug::RegisterRequest *request,
                   ::fixbug::RegisterResponse *response,
                   ::google::protobuf::Closure *done)
    {
        uint32_t id = request->id();
        string name = request->name();
        string pwd = request->pwd();
        bool resgister_result = Resgister(id, name, pwd);
        fixbug::ResultCode *code = response->mutable_result();
        code->set_errcode(resgister_result ? 0 : 1);
        code->set_errmsg(resgister_result ? "resgister success!" : "resgister error!");
        response->set_sucess(resgister_result);
        done->Run();
    }
};

int main(int argc , char** argv)
{
    newlog();
    MprpcApplication::Init(argc, argv);
     RpcProvider provider;
    provider.NotifyService(new UserService());
    provider.Run();
    return 0;
}