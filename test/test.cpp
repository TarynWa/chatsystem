#include"AsyncLogging.hpp"
#include<filesystem>
#include"nwl/EventLoop.hpp"
#include"nwl/InetAddress.hpp"
#include"nwl/TcpServer.hpp"
namespace fs = std::filesystem;
wangt::AsyncLogging *g_asyncLog=nullptr;
void asyncwrite(const string& info){
    g_asyncLog->append(info);
}
void asyncflush(){
    g_asyncLog->flush();
}

void connectedCallback(const nwl::TcpConnPtr& conn){
    if(conn->connected()){
        WT_LOG_INFO<<"New connection from "<<conn->peerAddress().toIpPort();
    }else{
        WT_LOG_INFO<<"Connection "<<conn->name()<<" is down";
    }
}

void messageCallback(const nwl::TcpConnPtr& conn,nwl::Buffer* buf,nwl::Timestamp time){
    std::string msg(buf->retrieveAllAsString());
    WT_LOG_INFO<<"Received "<<msg.size()<<" bytes from "<<conn->peerAddress().toIpPort()<<" at "<<time.toString();
    conn->send("I have received your message: "+msg);
}

int main(){
    //测试日志模块
    string path = "//home/wangt/chat/logmsg/test/";
    if (!fs::exists(path))
    {
        fs::create_directories(path);
    }
    g_asyncLog = new wangt::AsyncLogging(path, 1024 * 1024 * 10);
    g_asyncLog->start();
    wangt::Logger::setOutput(asyncwrite);
    wangt::Logger::setFlush(asyncflush);
    WT_LOG_INFO<<"AsyncLogging test";
    WT_LOG_INFO<<"AsyncLogging test endl";
    //测试网络模块
    WT_LOG_INFO<<"Network test";
    short port = 8000;
    string ip = "127.0.0.1";
    nwl::EventLoop loop;
    nwl::InetAddress addr(port,ip);
    nwl::TcpServer server(&loop,addr,"test");
    server.setConnectionCallback(connectedCallback);
    server.setMessageCallback(messageCallback);
    server.setThreadNum(4);
    server.start();
    loop.loop();
    return 0;
}