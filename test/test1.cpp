#include"AsyncLogging.hpp"
#include<filesystem>
#include<unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
namespace fs = std::filesystem;
wangt::AsyncLogging *g_asyncLog=nullptr;
void asyncwrite(const string& info){
    g_asyncLog->append(info);
}
void asyncflush(){
    g_asyncLog->flush();
}
int main(){
    //测试日志模块
    string path = "//home/wangt/chat/logmsg/test1/";
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
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) return -1;
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1)
    {
        close(sockfd);
        return -1;
    }
    WT_LOG_INFO << "Connected A to " << ip << ":" << port;
    string message = "Hello, Server!";
    send(sockfd, message.c_str(), message.size(), 0);
    char buffer[1024];
    ssize_t bytesRead = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead > 0)
    {
        buffer[bytesRead] = '\0';
        WT_LOG_INFO << "Received from server: " << buffer;
    }
    close(sockfd);
    return 0;
}