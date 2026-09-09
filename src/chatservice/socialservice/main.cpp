// SocialService(S0 好友+黑名单)入口: MprpcApplication::Init(-i conf) → DbPool(im_social)
// → NotifyService → Run。启动: ./bin/social_server -i conf/social.conf
// S0 只落 MySQL(不接 Redis; friend_requests.id 自增, 无需雪花)。鉴权信任调用方 uid。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "base/db_pool.h"
#include "mprpcapplication.h"
#include "mprpcconfig.h"
#include "rpcprovider.h"
#include "socialservice/socialservice_impl.h"

namespace dao = chatservice::dao;

int main(int argc, char** argv) {
  MprpcApplication::Init(argc, argv);
  MprpcConfig& cfg = MprpcApplication::GetConfig();

  unsigned mysqlport = (unsigned)atol(cfg.Load("mysqlport").c_str());
  if (mysqlport == 0) mysqlport = 3306;
  bool ok = dao::DbPool::instance().init(
      cfg.Load("mysqlip").empty() ? "localhost" : cfg.Load("mysqlip"),
      mysqlport, cfg.Load("mysqluser"), cfg.Load("mysqlpassword"),
      cfg.Load("mysqldb").empty() ? "im_social" : cfg.Load("mysqldb"),
      atoi(cfg.Load("mysqlpool_min").c_str()) > 0
          ? atoi(cfg.Load("mysqlpool_min").c_str())
          : 2,
      atoi(cfg.Load("mysqlpool_max").c_str()) > 0
          ? atoi(cfg.Load("mysqlpool_max").c_str())
          : 8);
  if (!ok) {
    fprintf(stderr, "[social_server] mysql init failed: %s\n",
            dao::DbPool::instance().lastErr().c_str());
    return 1;
  }

  printf("[social_server] db ready: rpc=%s:%s\n", cfg.Load("rpcserverip").c_str(),
         cfg.Load("rpcserverport").c_str());

  RpcProvider provider;
  provider.NotifyService(new chatservice::SocialServiceImpl());
  provider.Run();  // 阻塞: 注册 zk + 启动 nwl 事件循环
  return 0;
}
