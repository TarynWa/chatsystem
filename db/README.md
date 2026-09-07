## 🗄️ 分布式IM系统 - 数据库设计方案

基于您的C++自研架构，我为您设计完整的数据库方案。**不涉及代码实现**，聚焦于表结构设计、分库分表策略、索引优化和数据生命周期管理。

---

## 一、数据库整体规划

### 1.1 数据库拆分策略

```
┌─────────────────────────────────────────────────────────────┐
│                      数据库集群                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────────┐  ┌──────────────────┐  ┌───────────┐│
│  │  im_auth         │  │  im_message      │  │ im_social ││
│  │  (认证库)         │  │  (消息库)         │  │ (社交库)   ││
│  ├──────────────────┤  ├──────────────────┤  ├───────────┤│
│  │ • users          │  │ • messages_*     │  │ • friends ││
│  │ • user_devices   │  │ • group_messages │  │ • blacklist││
│  │ • sessions       │  │ • offline_msgs   │  │ • groups  ││
│  │ • audit_logs     │  │ • unread_counts  │  │ • group_  ││
│  │                  │  │ • read_status    │  │   members ││
│  │                  │  │                  │  │ • group_  ││
│  │                  │  │                  │  │   applies ││
│  └──────────────────┘  └──────────────────┘  └───────────┘│
│                                                             │
│  分库策略: 业务域隔离                                        │
│  优点: 独立扩展/故障隔离/权限分离                            │
│  缺点: 跨库查询需要应用层处理                               │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 存储引擎选择

| 数据类型 | 存储引擎 | 理由 |
|---------|---------|------|
| **用户数据** | InnoDB | 事务支持/行锁/高并发 |
| **消息数据** | InnoDB | 支持高并发写入/崩溃恢复 |
| **好友关系** | InnoDB | 事务保证一致性 |
| **群组数据** | InnoDB | 事务保证一致性 |
| **审计日志** | InnoDB | 支持大量写入/查询 |
| **离线消息** | InnoDB | 快速写入/删除 |
| **全文检索** | Elasticsearch | 独立存储/全文检索 |

---

## 二、认证库 (im_auth)

### 2.1 用户表 (users)

**设计要点**：
- 核心用户信息存储
- 支持软删除（保留历史数据）
- 密码使用bcrypt加密（cost=10）
- 多字段唯一约束（用户名/邮箱/手机号）
- 包含用户状态管理（在线/离线/忙碌/勿扰）

**关键字段**：
```yaml
字段设计:
  用户标识: id (BIGINT自增), username (唯一)
  认证信息: password (bcrypt加密)
  个人信息: nickname, avatar, email, phone, gender, birthday
  状态管理: status (0离线/1在线/2忙碌/3勿扰)
  权限控制: role (0普通/1管理员/2超级管理员)
  安全追踪: last_login_at, last_ip, device_info
  审计字段: created_at, updated_at, deleted_at (软删除)

索引策略:
  主键: id
  唯一索引: username, email, phone
  普通索引: status, created_at
  组合索引: (status, last_login_at) 用于查询活跃用户
```

### 2.2 用户设备表 (user_devices)

**设计要点**：
- 管理用户多端登录
- 记录设备推送Token（用于离线推送）
- 追踪设备在线状态
- 支持多端同时在线（最多5台）

**关键字段**：
```yaml
字段设计:
  设备标识: id, device_id (唯一标识)
  用户关联: user_id
  设备信息: device_name, device_type (iOS/Android/Web/PC)
  系统信息: os_version, app_version
  推送配置: push_token (APNS/FCM)
  状态管理: online_status, last_active_at
  审计字段: created_at, updated_at

索引策略:
  主键: id
  唯一索引: (user_id, device_id)
  普通索引: online_status, last_active_at
```

### 2.3 会话表 (sessions)

**设计要点**：
- 管理用户登录会话
- 存储JWT Token信息
- 支持Token刷新机制
- 自动过期清理（定期任务）

**关键字段**：
```yaml
字段设计:
  会话标识: id, token (JWT)
  用户关联: user_id, device_id
  刷新机制: refresh_token, refresh_expires_at
  过期控制: expires_at
  状态管理: status (0失效/1有效)
  安全信息: ip, user_agent
  审计字段: created_at, updated_at

索引策略:
  主键: id
  唯一索引: token (前缀索引, 255字符)
  普通索引: (user_id, device_id), expires_at, status
```

### 2.4 审计日志表 (audit_logs)

**设计要点**：
- 记录所有敏感操作
- 支持安全审计和溯源
- 使用JSON存储操作详情（灵活扩展）
- 定期归档（保留6个月）

**关键字段**：
```yaml
字段设计:
  日志标识: id
  操作主体: user_id, username
  操作信息: action (login/logout/register/update), resource_type, resource_id
  操作详情: detail (JSON格式)
  安全信息: ip, user_agent
  执行结果: status (0失败/1成功), error_msg
  审计字段: created_at

索引策略:
  主键: id
  普通索引: (user_id), action, created_at
  组合索引: (user_id, action, created_at) 用于用户行为分析
  分区策略: 按月份分区 (保留6个月)
```

---

## 三、消息库 (im_message)

### 3.1 单聊消息表 (messages_YYYYMM)

**设计要点**：
- **按月分表**（messages_202601, messages_202602...）
- 支持海量消息存储（亿级）
- 消息ID使用雪花算法（全局唯一）
- 支持消息状态管理（未读/已读/撤回/删除）
- 媒体消息支持（图片/语音/视频/文件）

**关键字段**：
```yaml
字段设计:
  消息标识: id, msg_id (雪花算法生成)
  收发双方: from_user, to_user
  消息内容: msg_type (0文本/1图片/2语音/3视频/4文件/5位置/6表情)
  content (文本或JSON), media_urls (JSON)
  消息状态: status (0未读/1已读/2已撤回/3已删除)
  已读追踪: read_at
  撤回追踪: recall_at
  审计字段: created_at, updated_at

索引策略:
  主键: id
  唯一索引: msg_id
  普通索引: from_user, to_user, created_at
  组合索引: 
    - (to_user, created_at) 用于拉取历史消息
    - (from_user, to_user, created_at) 用于查询两人聊天记录
    - (to_user, status, created_at) 用于查询未读消息

分表策略:
  表名: messages_YYYYMM (按月)
  优点: 便于归档/清理/备份
  缺点: 跨月查询需要应用层路由
```

### 3.2 群聊消息表 (group_messages)

**设计要点**：
- 与单聊分开存储（便于独立扩展）
- 支持@用户功能（at_users字段）
- 群消息不需要分表（群数量相对少）
- 支持群消息撤回

**关键字段**：
```yaml
字段设计:
  消息标识: id, msg_id
  群组关联: group_id
  发送者: from_user
  消息内容: msg_type, content, media_urls
  群功能: at_users (JSON数组)
  消息状态: status (0正常/1已撤回/2已删除)
  撤回追踪: recall_at
  审计字段: created_at, updated_at

索引策略:
  主键: id
  唯一索引: msg_id
  普通索引: group_id, from_user, created_at
  组合索引: (group_id, created_at) 用于拉取群历史消息
```

### 3.3 离线消息表 (offline_messages)

**设计要点**：
- 存储用户离线期间的消息
- 用户上线后批量拉取
- 拉取后立即删除或标记已读
- 定期清理过期离线消息（7天）

**关键字段**：
```yaml
字段设计:
  离线标识: id
  消息标识: msg_id (关联原始消息)
  接收者: user_id
  发送者: from_user
  消息内容: msg_type, content, media_urls
  群组标识: is_group, group_id (如果是群消息)
  状态管理: status (0未拉取/1已拉取)
  审计字段: created_at, delivered_at

索引策略:
  主键: id
  普通索引: user_id, created_at, status
  组合索引: (user_id, status) 用于拉取离线消息
  分区策略: 按月份分区 (保留7天)
```

### 3.4 消息已读状态表 (message_read_status)

**设计要点**：
- 记录每条消息的已读用户
- 支持多端同步（一个用户多设备）
- 用于显示"已读"状态

**关键字段**：
```yaml
字段设计:
  记录标识: id
  消息标识: msg_id
  已读用户: user_id
  已读时间: read_at

索引策略:
  主键: id
  唯一索引: (msg_id, user_id)
  普通索引: user_id
```

### 3.5 未读计数表 (unread_counts)

**设计要点**：
- 缓存用户的未读消息数
- 支持单聊和群聊分别计数
- 实时更新（MQ异步刷新）
- 用于显示红点提示

**关键字段**：
```yaml
字段设计:
  记录标识: id
  用户标识: user_id
  目标标识: target_id (好友ID或群组ID)
  目标类型: target_type (1单聊/2群聊)
  未读数: unread_count
  最后已读: last_read_msg_id
  审计字段: updated_at

索引策略:
  主键: id
  唯一索引: (user_id, target_id, target_type)
  普通索引: user_id
```

---

## 四、社交库 (im_social)

### 4.1 好友关系表 (friends)

**设计要点**：
- **分库分表**（16库 × 64表）
- 支持好友状态管理（待确认/已确认/已拒绝/已删除）
- 支持备注名和扩展信息
- 记录申请和确认时间

**关键字段**：
```yaml
字段设计:
  关系标识: id
  双方用户: user_id, friend_id
  关系状态: status (0待确认/1已确认/2已拒绝/3已删除)
  关系信息: remark (备注名), extra (JSON扩展)
  申请信息: apply_msg, apply_at
  确认信息: confirm_at
  审计字段: created_at, updated_at

索引策略:
  主键: id
  唯一索引: (user_id, friend_id)
  普通索引: user_id, friend_id, status
  组合索引: (user_id, status, created_at) 用于查询好友列表

分库分表策略:
  路由键: user_id
  库数量: 16 (im_social_0 ~ im_social_15)
  表数量: 64 (friends_0 ~ friends_63)
  路由算法: 库 = hash(user_id) % 16, 表 = hash(user_id) % 64
  数据分布: 相同user_id的数据在同一张表
```

### 4.2 黑名单表 (blacklist)

**设计要点**：
- 记录用户屏蔽的好友
- 屏蔽后双方不能互相发送消息
- 支持解封操作

**关键字段**：
```yaml
字段设计:
  记录标识: id
  用户标识: user_id
  被屏蔽用户: blocked_user_id
  屏蔽原因: reason
  审计字段: created_at

索引策略:
  主键: id
  唯一索引: (user_id, blocked_user_id)
  普通索引: user_id
```

### 4.3 好友申请表 (friend_requests)

**设计要点**：
- 记录所有好友申请
- 支持申请状态管理
- 申请过期自动失效（3天）
- 用于消息通知和重试

**关键字段**：
```yaml
字段设计:
  申请标识: id
  申请人: from_user
  接收人: to_user
  申请信息: apply_msg
  申请状态: status (0待处理/1已接受/2已拒绝/3已过期)
  处理信息: handled_at, handler_id
  过期时间: expire_at
  审计字段: created_at, updated_at

索引策略:
  主键: id
  普通索引: from_user, to_user, status
  组合索引: (to_user, status, created_at) 用于查询待处理申请
```

### 4.4 群组表 (groups)

**设计要点**：
- **分库分表**（8库 × 32表）
- 群组基础信息存储
- 支持群组设置（验证/搜索/邀请）
- 记录群成员数量（冗余字段）

**关键字段**：
```yaml
字段设计:
  群组标识: id, group_id (唯一)
  群组信息: name, avatar, notice, introduction
  群主: owner_id
  规模: member_count, max_members (默认500)
  群设置: join_verify, allow_invite, allow_search
  状态管理: status (0已解散/1正常)
  审计字段: dismissed_at, created_at, updated_at

索引策略:
  主键: id
  唯一索引: group_id
  普通索引: owner_id, name, status
  组合索引: (status, created_at) 用于查询活跃群组

分库分表策略:
  路由键: group_id
  库数量: 8 (im_social_0 ~ im_social_7)
  表数量: 32 (groups_0 ~ groups_31)
  路由算法: 库 = hash(group_id) % 8, 表 = hash(group_id) % 32
```

### 4.5 群成员表 (group_members)

**设计要点**：
- **分库分表**（8库 × 32表）
- 记录群组成员信息
- 支持角色管理（普通/管理员/群主）
- 支持禁言功能
- 记录最后已读时间

**关键字段**：
```yaml
字段设计:
  记录标识: id
  群组标识: group_id
  用户标识: user_id
  成员角色: role (0普通/1管理员/2群主)
  群内信息: nickname (群内昵称)
  加入方式: join_way (0邀请/1申请/2直接加入)
  禁言控制: mute_expire_at
  状态管理: status (0已退出/1正常/2被踢出)
  阅读追踪: last_read_time
  审计字段: join_at, leave_at, created_at, updated_at

索引策略:
  主键: id
  唯一索引: (group_id, user_id)
  普通索引: user_id, role, status
  组合索引: (group_id, status) 用于查询群成员列表

分库分表策略:
  路由键: group_id (与groups表一致)
  库数量: 8
  表数量: 32
  路由算法: 与groups表相同
  优点: 关联查询时可在同一库
```

### 4.6 群申请表 (group_applies)

**设计要点**：
- 记录加群申请
- 支持邀请机制（inviter_id）
- 申请状态管理
- 申请过期自动失效

**关键字段**：
```yaml
字段设计:
  申请标识: id
  群组标识: group_id
  申请人: applicant_id
  邀请人: inviter_id (如果是邀请加入)
  申请信息: apply_msg
  申请状态: status (0待处理/1已通过/2已拒绝/3已过期)
  处理信息: handler_id, handled_at
  过期时间: expire_at
  审计字段: created_at, updated_at

索引策略:
  主键: id
  普通索引: group_id, applicant_id, status
  组合索引: (group_id, status, created_at) 用于查询待处理申请
```

---

## 五、分库分表详细设计

### 5.1 分片策略总览

| 表名 | 分片键 | 库数量 | 表数量 | 总表数 | 算法 |
|------|--------|--------|--------|--------|------|
| friends | user_id | 16 | 64 | 1024 | hash(user_id) % 1024 |
| blacklist | user_id | 16 | 64 | 1024 | hash(user_id) % 1024 |
| groups | group_id | 8 | 32 | 256 | hash(group_id) % 256 |
| group_members | group_id | 8 | 32 | 256 | hash(group_id) % 256 |
| messages_* | to_user | 16 | 64 | 1024 | hash(to_user) % 1024 |

### 5.2 路由计算示例

```yaml
好友表路由 (user_id = 12345):
  库索引: hash(12345) % 16 = 5 → im_social_5
  表索引: hash(12345) % 64 = 23 → friends_23
  完整表名: im_social_5.friends_23

群组表路由 (group_id = 67890):
  库索引: hash(67890) % 8 = 2 → im_social_2
  表索引: hash(67890) % 32 = 10 → groups_10
  完整表名: im_social_2.groups_10

消息表路由 (to_user = 12345, 2026年1月):
  库索引: hash(12345) % 16 = 5 → im_message_5
  表索引: hash(12345) % 64 = 23 → messages_202601_23
  完整表名: im_message_5.messages_202601_23
```

### 5.3 跨库查询处理

```yaml
场景1: 查询用户的好友列表
  方案: 根据user_id路由到具体库表
  查询: 单库单表 (性能最优)
  
场景2: 查询两个用户的共同好友
  方案: 分别查询两个用户的表，然后取交集
  查询: 2次查询 → 应用层合并 (性能中等)
  
场景3: 批量查询多个用户信息
  方案: 根据user_id哈希分组，分别查询各库
  查询: 多库并发查询 → 应用层汇总 (性能较低)
  
场景4: 查询用户的所有群组
  方案: 根据user_id在group_members表查询
  查询: 需要先根据user_id无法确定group_id路由
  优化: 冗余一张用户-群组映射表 (按user_id分片)
  或: 使用索引表/ES支持反向查询
```

### 5.4 全局唯一ID生成

```yaml
雪花算法 (Snowflake):
  方案: 64位ID (41位时间戳 + 10位机器ID + 12位序列号)
  
  时间戳: 41位 (可用69年)
  机器ID: 10位 (支持1024个节点)
  序列号: 12位 (单节点每毫秒4096个ID)
  
  优点: 
    - 全局唯一
    - 趋势递增 (便于排序)
    - 高性能 (本地生成无网络开销)
    - 包含时间信息 (可解析)
  
  应用场景:
    - msg_id (消息ID)
    - group_id (群组ID)
    - 订单ID/事件ID

实现要点:
  - 机器ID从Consul获取 (保证唯一)
  - 时间回拨处理 (等待或使用备用序列)
  - 时钟同步 (NTP定期同步)
```

---

## 六、索引优化策略

### 6.1 索引设计原则

```yaml
原则1: 选择性高的字段建索引
  - 用户表: username, email, phone (唯一性高)
  - 消息表: msg_id (唯一性高)
  - 不建索引: gender (选择性低)

原则2: 查询频繁的字段建组合索引
  - 消息表: (to_user, created_at) 用于翻页查询历史消息
  - 好友表: (user_id, status, created_at) 用于查询好友列表

原则3: 避免过多索引
  - 每张表索引数 < 5个
  - 评估索引收益 (查询性能 vs 写入性能)

原则4: 使用覆盖索引减少回表
  - 消息表: (to_user, status, msg_id, content) 覆盖未读消息查询
  - 用户表: (username, password, id) 覆盖登录验证

原则5: 使用前缀索引减少空间
  - 消息表: token字段使用前缀索引 (255字符)
  - 内容字段: 使用全文索引 (ES)
```

### 6.2 核心查询优化

```yaml
查询1: 拉取历史消息 (分页)
  SQL: SELECT * FROM messages_202601 
       WHERE to_user = ? AND created_at < ? 
       ORDER BY created_at DESC LIMIT 20
  索引: (to_user, created_at) DESC
  覆盖: 使用覆盖索引减少回表

查询2: 查询未读消息
  SQL: SELECT COUNT(*) FROM messages_202601 
       WHERE to_user = ? AND status = 0
  索引: (to_user, status)
  优化: 使用未读计数表缓存

查询3: 查询好友列表
  SQL: SELECT * FROM friends 
       WHERE user_id = ? AND status = 1
       ORDER BY created_at DESC
  索引: (user_id, status, created_at)
  覆盖: 覆盖索引避免回表

查询4: 查询群成员
  SQL: SELECT * FROM group_members 
       WHERE group_id = ? AND status = 1
  索引: (group_id, status)
  优化: 缓存成员列表 (Redis)

查询5: 搜索用户
  SQL: SELECT * FROM users 
       WHERE username LIKE '%keyword%'
  索引: 无法使用索引 (全表扫描)
  优化: 使用Elasticsearch全文检索
```

### 6.3 慢查询监控

```yaml
慢查询阈值: 100ms
监控指标:
  - 执行时间 > 100ms
  - 扫描行数 > 1000行
  - 使用临时表/文件排序

优化手段:
  1. 分析执行计划 (EXPLAIN)
  2. 添加合适的索引
  3. 重写SQL语句
  4. 调整数据库参数
  5. 使用缓存层

工具:
  - 慢查询日志 (记录所有慢SQL)
  - pt-query-digest (分析慢查询)
  - MySQL Performance Schema
  - 自研监控系统 (实时告警)
```

---

## 七、数据生命周期管理

### 7.1 数据分级存储

```yaml
热数据 (Hot Data):
  存储: MySQL (InnoDB)
  访问: 频繁读写
  保留: 7天内
  示例: 最近7天的消息、活跃用户信息

温数据 (Warm Data):
  存储: MySQL (归档表) / 腾讯云TDSQL
  访问: 偶尔查询
  保留: 30天内
  示例: 7-30天的消息、历史好友关系

冷数据 (Cold Data):
  存储: Elasticsearch / OSS对象存储
  访问: 极少查询 (全文检索)
  保留: 永久
  示例: 30天前的消息、历史审计日志

归档策略:
  每天凌晨3点执行归档任务
  热数据 → 温数据 (7天前)
  温数据 → 冷数据 (30天前)
```

### 7.2 定期清理策略

```yaml
清理1: 离线消息
  - 频率: 每小时
  - 条件: 已拉取 > 24小时
  - 操作: DELETE FROM offline_messages WHERE status=1 AND created_at < DATE_SUB(NOW(), INTERVAL 1 DAY)

清理2: 会话Token
  - 频率: 每小时
  - 条件: 过期时间 < NOW() - 7天
  - 操作: DELETE FROM sessions WHERE expires_at < DATE_SUB(NOW(), INTERVAL 7 DAY)

清理3: 审计日志
  - 频率: 每天
  - 条件: created_at < NOW() - 180天
  - 操作: 导出到OSS → 删除原数据

清理4: 消息表
  - 频率: 每月1日
  - 条件: 月份 < 当前月份 - 3
  - 操作: 切换到新表 → 归档旧表

清理5: 好友申请
  - 频率: 每天
  - 条件: status=0 AND created_at < NOW() - 3天
  - 操作: UPDATE friend_requests SET status=3 WHERE status=0 AND created_at < DATE_SUB(NOW(), INTERVAL 3 DAY)
```

### 7.3 数据备份策略

```yaml
全量备份:
  - 频率: 每天凌晨2点
  - 方式: mysqldump / XtraBackup
  - 保留: 7天

增量备份:
  - 频率: 每6小时
  - 方式: binlog备份
  - 保留: 7天

归档备份:
  - 频率: 每月1日
  - 方式: 导出到OSS
  - 保留: 永久

恢复策略:
  - 全量恢复: < 2小时 (100GB数据)
  - 增量恢复: < 30分钟 (应用binlog)
  - 数据校验: 恢复后执行数据验证
  - 演练: 每季度进行恢复演练
```

---

## 八、高可用与容灾

### 8.1 MySQL高可用架构

```yaml
架构: 一主两从 (1 Master + 2 Slave)
  主库: 负责所有写操作 + 部分读操作
  从库1: 负责读操作 (业务查询)
  从库2: 负责读操作 + 备份 (离线分析)

复制模式: 异步复制 (默认) / 半同步复制 (关键业务)
  优点: 高性能
  缺点: 主从延迟 (100ms-1s)
  优化: 使用半同步复制 (保证数据不丢失)

切换方案: MHA (MySQL High Availability)
  检测: 每3秒探测主库存活
  切换: 主库故障 → 自动提升从库为主库
  切换时间: < 30秒
  数据丢失: < 1秒 (半同步复制)

读写分离:
  写操作: 路由到主库
  读操作: 路由到从库 (负载均衡)
  事务内: 强制主库 (保证一致性)
  延迟敏感: 主库 (如: 查询刚插入的数据)
```

### 8.2 Redis高可用架构

```yaml
架构: 哨兵模式 (1 Master + 2 Slave + 3 Sentinel)
  主节点: 负责所有写操作
  从节点: 负责读操作 (缓存查询)
  哨兵: 监控/故障检测/自动切换

切换方案: 自动故障转移
  检测: 哨兵每2秒探测主库
  切换: 主库故障 → 选举新主库
  切换时间: < 10秒
  数据丢失: < 1秒 (异步复制)

分片策略: Redis Cluster (如果数据量 > 100GB)
  分片数量: 6节点 (3主3从)
  虚拟槽: 16384个槽
  路由: key → CRC16 → 槽 → 节点
```

### 8.3 数据一致性保障

```yaml
场景1: 好友关系 + 缓存一致性
  方案: 双删策略 + MQ广播
  Step1: 删除Redis缓存
  Step2: 更新MySQL数据库
  Step3: 延迟500ms
  Step4: 再次删除Redis缓存
  Step5: 发送MQ消息通知其他服务更新

场景2: 消息发送 + 未读计数一致性
  方案: 最终一致性 (本地消息表 + 补偿)
  Step1: 存储消息到MySQL (事务)
  Step2: 异步更新未读计数 (MQ)
  Step3: 补偿任务定期扫描 (每5分钟)
  Step4: 修复不一致数据

场景3: 群成员变更 + 成员列表一致性
  方案: 本地事务 + 消息广播
  Step1: 开启事务
  Step2: 更新群成员表
  Step3: 更新群成员计数 (groups.member_count)
  Step4: 提交事务
  Step5: MQ广播成员变更事件
  Step6: 各服务更新本地缓存

场景4: 跨库分布式事务 (极少数场景)
  方案: TCC模式 (Try-Confirm-Cancel)
  示例: 加好友 + 更新好友列表 + 发送通知
  Try: 创建待确认好友记录 (status=0)
  Confirm: 更新状态为已确认 + 更新缓存 + 发送通知
  Cancel: 删除好友记录 (status=2)
```

---

## 九、性能基准

### 9.1 性能目标

| 指标 | 目标值 | 备注 |
|------|--------|------|
| **查询延迟 (P99)** | < 50ms | 缓存命中 |
| **查询延迟 (P99)** | < 200ms | 直接查询DB |
| **写入延迟 (P99)** | < 50ms | 单条写入 |
| **批量写入 (QPS)** | 10000+ | 消息写入 |
| **并发连接数** | 1000+ | DB连接池 |
| **数据量** | 10亿+ | 消息表 |

### 9.2 容量规划

```yaml
消息表容量:
  日活用户: 10万
  每日消息量: 500万条
  单条消息大小: 1KB (平均)
  每日存储增量: 5GB
  年存储增量: 1.8TB
  保留策略: 热数据7天 (35GB), 温数据30天 (150GB)
  总存储: 约2TB/年

用户表容量:
  用户数: 100万
  单条记录大小: 1KB
  总存储: 1GB

好友关系容量:
  好友关系数: 1000万 (人均10好友)
  单条记录大小: 200B
  总存储: 2GB

群组容量:
  群组数: 10万
  群成员: 100万 (人均10个群)
  总存储: 200MB (群组) + 2GB (群成员)
```

---

## 十、监控与运维

### 10.1 监控指标

```yaml
数据库监控:
  - 连接数: 活跃连接/空闲连接
  - QPS: 每秒查询数
  - 慢查询: 慢SQL数量
  - 主从延迟: 复制延迟时间
  - 表空间: 各表大小
  - 索引命中率: 缓存命中率
  - Buffer Pool: 命中率/使用率

告警规则:
  连接数 > 80% → 告警
  慢查询 > 10/min → 告警
  主从延迟 > 5s → 告警
  表空间 > 80% → 告警 (需扩容)
  错误日志 > 100/min → 告警
  死锁 > 10/min → 告警

性能指标:
  查询响应时间: P50/P95/P99
  写入响应时间: P50/P95/P99
  QPS趋势: 实时监控
  资源使用率: CPU/内存/磁盘IO/网络IO
```

### 10.2 运维任务

```yaml
日常巡检:
  - 检查主从状态: SHOW SLAVE STATUS
  - 检查表空间: 是否达到阈值
  - 检查慢查询: 优化慢SQL
  - 检查错误日志: 排查异常
  - 检查备份状态: 备份是否成功

每周任务:
  - 分析慢查询日志
  - 优化索引 (根据查询模式)
  - 查看磁盘空间 (预测扩容)
  - 查看数据库性能报表

每月任务:
  - 执行数据归档
  - 执行数据清理
  - 检查数据备份完整性
  - 容量规划 (预测未来需求)
  - 数据库参数优化 (根据运行情况)

每季度任务:
  - 故障演练 (模拟主从切换)
  - 恢复演练 (从备份恢复)
  - 安全检查 (权限审计)
  - 版本升级 (MySQL小版本)
  - 性能压测 (验证容量)
```

---

## 📊 总结

### 核心设计亮点

```yaml
1. 业务域隔离
   - 三库分离 (auth/message/social)
   - 独立扩展/故障隔离

2. 分库分表策略
   - 16库 × 64表 (1024表)
   - hash分片 / 按需扩容

3. 数据生命周期管理
   - 热/温/冷三级存储
   - 自动归档/清理

4. 高性能索引设计
   - 组合索引优化查询
   - 覆盖索引减少回表

5. 高可用架构
   - 一主两从 + MHA自动切换
   - 读写分离提升性能

6. 数据一致性保障
   - TCC分布式事务
   - 最终一致性方案
   - 定期补偿机制
```

### 与架构的融合

```yaml
存储层与业务层的配合:
  - MySQL: 主存储 (InnoDB引擎)
  - Redis: L2缓存 (降低DB压力)
  - ES: 全文检索 (历史消息查询)
  - 本地缓存: L1缓存 (极速访问)

分库分表与业务路由:
  - 业务层根据user_id计算分片
  - 通过路由层透明访问
  - 跨库查询在应用层处理

数据一致性与MQ配合:
  - MQ异步更新缓存 (最终一致)
  - 补偿任务修复不一致数据
  - 乐观锁处理并发更新
```

这个数据库设计方案与您的C++架构完美配合，支持**亿级用户**和**万亿级消息**的存储，同时保证了**高性能**、**高可用**和**可扩展性**。需要我详细展开某个表的字段设计或索引优化吗？