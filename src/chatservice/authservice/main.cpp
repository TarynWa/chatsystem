
// AuthService 入口: MprpcApplication::Init(-i conf) → 连接池初始化 → NotifyService → Run
// 启动: ./auth_server -i conf/auth.conf
#include <cstdio>
#include <cstdlib>
#include <string>

#include "auth.pb.h"
#include "authservice/authservice_impl.h"
#include "authservice/cache/redis_pool.h"
#include "authservice/dao/db_pool.h"
#include "authservice/service/session_service.h"
#include "mprpcapplication.h"
#include "mprpcconfig.h"
#include "rpcprovider.h"

namespace dao = chatservice::dao;
namespace cache = chatservice::cache;

namespace {

long loadInt(MprpcConfig& cfg, const char* key, long def) {
  const std::string& s = cfg.Load(key);
  return s.empty() ? def : atol(s.c_str());
}

} // namespace

int main(int argc, char** argv) {
  // 启动参数解析(-i conf), 内含 rpcserverip/port 与 zookeeper 地址
  MprpcApplication::Init(argc, argv);
  MprpcConfig& cfg = MprpcApplication::GetConfig();

  // ---- MySQL (im_auth) ----
  bool ok = dao::DbPool::instance().init(
      cfg.Load("mysqlip").empty() ? "localhost" : cfg.Load("mysqlip"),
      (unsigned)loadInt(cfg, "mysqlport", 3306), cfg.Load("mysqluser"),
      cfg.Load("mysqlpassword"), cfg.Load("mysqldb"),
      (int)loadInt(cfg, "mysqlpool_min", 2), (int)loadInt(cfg, "mysqlpool_max", 8));
  if (!ok) {
    fprintf(stderr, "[auth_server] mysql init failed: %s\n",
            dao::DbPool::instance().lastErr().c_str());
    return 1;
  }

  // ---- Redis (会话/防爆破) ----
  ok = cache::RedisPool::instance().init(
      cfg.Load("redisip").empty() ? "127.0.0.1" : cfg.Load("redisip"),
      (int)loadInt(cfg, "redisport", 6379), (int)loadInt(cfg, "redisdb", 0),
      (int)loadInt(cfg, "redispool_min", 1),
      (int)loadInt(cfg, "redispool_max", 4));
  if (!ok) {
    fprintf(stderr, "[auth_server] redis init failed: %s\n",
            cache::RedisPool::instance().lastErr().c_str());
    return 1;
  }

  // ---- 运行参数 ----
  chatservice::service::Policy pol;
  pol.accessSec = loadInt(cfg, "access_expire_seconds", 7200);
  pol.refreshSec = loadInt(cfg, "refresh_expire_days", 7) * 24L * 3600;
  pol.maxDevices = (int)loadInt(cfg, "max_devices_per_user", 5);
  pol.kickOldest = cfg.Load("device_limit_policy").empty() ||
                   cfg.Load("device_limit_policy") == "kick_oldest";
  pol.maxFail = (int)loadInt(cfg, "brute_force_max_fail", 5);
  pol.lockMinutes = (int)loadInt(cfg, "brute_force_lock_minutes", 30);

  printf("[auth_server] db/redis/policy ready: rpc=%s:%s\n",
         cfg.Load("rpcserverip").c_str(), cfg.Load("rpcserverport").c_str());

  RpcProvider provider;
  provider.NotifyService(new chatservice::AuthServiceImpl(pol));
  provider.Run();  // 阻塞: 注册 zk + 启动 nwl 事件循环
  return 0;
}
