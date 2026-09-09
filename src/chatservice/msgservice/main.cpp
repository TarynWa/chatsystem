// MsgService(S0 单聊)入口: MprpcApplication::Init(-i conf) → DbPool(im_message) → NotifyService → Run
// 启动: ./bin/msg_server -i conf/msg.conf
// S0 只落 MySQL(不接 Redis)。鉴权信任调用方 uid(见 README §接入形态)。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "base/db_pool.h"
#include "base/snowflake.h"
#include "mprpcapplication.h"
#include "mprpcconfig.h"
#include "msgservice/msgservice_impl.h"
#include "rpcprovider.h"

namespace dao = chatservice::dao;

int main(int argc, char** argv) {
  MprpcApplication::Init(argc, argv);
  MprpcConfig& cfg = MprpcApplication::GetConfig();

  unsigned mysqlport = (unsigned)atol(cfg.Load("mysqlport").c_str());
  if (mysqlport == 0) mysqlport = 3306;
  bool ok = dao::DbPool::instance().init(
      cfg.Load("mysqlip").empty() ? "localhost" : cfg.Load("mysqlip"),
      mysqlport, cfg.Load("mysqluser"), cfg.Load("mysqlpassword"),
      cfg.Load("mysqldb").empty() ? "im_message" : cfg.Load("mysqldb"),
      atoi(cfg.Load("mysqlpool_min").c_str()) > 0
          ? atoi(cfg.Load("mysqlpool_min").c_str())
          : 2,
      atoi(cfg.Load("mysqlpool_max").c_str()) > 0
          ? atoi(cfg.Load("mysqlpool_max").c_str())
          : 8);
  if (!ok) {
    fprintf(stderr, "[msg_server] mysql init failed: %s\n",
            dao::DbPool::instance().lastErr().c_str());
    return 1;
  }

  // 雪花默认 worker=1(单机进程唯一); 多进程部署时按实例配置不同 worker
  chatservice::common::Snowflake::instance().init(1);

  printf("[msg_server] db ready: rpc=%s:%s\n", cfg.Load("rpcserverip").c_str(),
         cfg.Load("rpcserverport").c_str());

  RpcProvider provider;
  provider.NotifyService(new chatservice::MsgServiceImpl());
  provider.Run();  // 阻塞: 注册 zk + 启动 nwl 事件循环
  return 0;
}
