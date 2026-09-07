#include "authservice/dao/device_dao.h"

#include "authservice/dao/sqlutil.h"

namespace chatservice::dao {

bool upsertDevice(MYSQL* c, uint64_t user_id, const model::DeviceInfo& dev) {
  if (!c) return false;
  std::string sql =
      "INSERT INTO user_devices "
      "(user_id,device_id,device_name,device_type,os_version,app_version,"
      " push_token,online_status,last_active_at) VALUES (" +
      std::to_string(user_id) + "," + q(c, dev.device_id) + "," +
      q(c, dev.device_name) + "," + std::to_string(dev.device_type) + "," +
      q(c, dev.os_version) + "," + q(c, dev.app_version) +
      ",'',1,NOW()) ON DUPLICATE KEY UPDATE device_name=VALUES(device_name),"
      "device_type=VALUES(device_type),os_version=VALUES(os_version),"
      "app_version=VALUES(app_version),online_status=1,last_active_at=NOW()";
  return mysql_real_query(c, sql.data(), sql.size()) == 0;
}

bool setDeviceOffline(MYSQL* c, uint64_t user_id, const std::string& device_id) {
  if (!c) return false;
  std::string sql = "UPDATE user_devices SET online_status=0 WHERE user_id=" +
                    std::to_string(user_id) + " AND device_id=" +
                    q(c, device_id);
  return mysql_real_query(c, sql.data(), sql.size()) == 0;
}

} // namespace chatservice::dao
