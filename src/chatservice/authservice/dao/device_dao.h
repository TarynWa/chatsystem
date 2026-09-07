#pragma once
#include <mysql/mysql.h>
#include <cstdint>
#include <string>

#include "authservice/model/user.h"

namespace chatservice::dao {

// 登记/刷新设备: 同名 (user_id,device_id) 幂等覆盖(ON DUPLICATE KEY), 置在线
bool upsertDevice(MYSQL* c, uint64_t user_id, const model::DeviceInfo& dev);

// 设备下线(kick/登出时清 user_devices.online_status)
bool setDeviceOffline(MYSQL* c, uint64_t user_id, const std::string& device_id);

} // namespace chatservice::dao
