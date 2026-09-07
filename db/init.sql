-- =====================================================================
-- 分布式 IM 系统 - 数据库初始化脚本
-- 依据: db/README.md《数据库设计方案》
-- 适用: MySQL 8.0+ (本地实测 8.4), InnoDB, utf8mb4
--
-- 说明:
--   1) 本脚本按「业务域隔离」初始化三个逻辑库 im_auth / im_message / im_social,
--      覆盖设计稿 §2/§3/§4 的全部基础表(15 张)。
--   2) 水平分库分表(friends/blacklist/groups/group_members/messages_*,
--      §5)属于上量后的部署扩展, 不在此默认建 1024+ 张物理表;
--      每张表注释里保留了该表的路由键/路由算法, 便于后续做分片生成器。
--   3) 全部 *_id / msg_id 用 BIGINT UNSIGNED, 与雪花算法(Snowflake)生成的
--      非负 ID 对齐; 跨库无外键, 引用完整性由应用层保证(分片场景惯例)。
--   4) 可重复执行: CREATE DATABASE IF NOT EXISTS + 每表 DROP IF EXISTS。
--      注意会清空同名表数据, 仅用于初始化。
--
-- 执行: mysql -uroot -p < db/init.sql
-- 校验: SHOW DATABASES; USE im_auth; SHOW TABLES;
-- =====================================================================

SET NAMES utf8mb4;

-- =====================================================================
-- 一、认证库 im_auth  (对应设计稿 §2)
--     单库不拆分。users 支持软删除, 密码 bcrypt。
-- =====================================================================
CREATE DATABASE IF NOT EXISTS im_auth
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE im_auth;

-- ---------------------------------------------------------------
-- 2.1 用户表 users
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS users;
CREATE TABLE users (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '用户ID',
  username      VARCHAR(64)     NOT NULL                COMMENT '用户名(唯一)',
  password      VARCHAR(255)    NOT NULL                COMMENT '密码(bcrypt加密, cost=10)',
  nickname      VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '昵称',
  avatar        VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '头像URL',
  email         VARCHAR(128)    NULL DEFAULT NULL       COMMENT '邮箱(唯一; 未绑定为NULL, 允许多个未绑定)',
  phone         VARCHAR(32)     NULL DEFAULT NULL       COMMENT '手机号(唯一; 未绑定为NULL, 允许多个未绑定)',
  gender        TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '性别 0未知/1男/2女',
  birthday      DATE            NULL                    COMMENT '生日',
  status        TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0离线/1在线/2忙碌/3勿扰',
  role          TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '角色 0普通/1管理员/2超级管理员',
  last_login_at DATETIME        NULL                    COMMENT '最近登录时间',
  last_ip       VARCHAR(45)     NOT NULL DEFAULT ''     COMMENT '最近登录IP(兼容IPv6)',
  device_info   VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '最近登录设备信息',
  created_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  updated_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  deleted_at    DATETIME        NULL                    COMMENT '软删除时间(非空=已删除)',
  PRIMARY KEY (id),
  UNIQUE KEY uk_username (username),
  UNIQUE KEY uk_email (email),
  UNIQUE KEY uk_phone (phone),
  KEY idx_status (status),
  KEY idx_created_at (created_at),
  KEY idx_status_last_login (status, last_login_at)      COMMENT '活跃用户查询'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='核心用户表(软删除)';

-- ---------------------------------------------------------------
-- 2.2 用户设备表 user_devices (多端登录, 上限5台由应用控制)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS user_devices;
CREATE TABLE user_devices (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '设备记录ID',
  user_id        BIGINT UNSIGNED NOT NULL                COMMENT '用户ID(users.id)',
  device_id      VARCHAR(64)     NOT NULL                COMMENT '设备唯一标识',
  device_name    VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '设备名称',
  device_type    TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '设备类型 0未知/1iOS/2Android/3Web/4PC',
  os_version     VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '操作系统版本',
  app_version    VARCHAR(32)     NOT NULL DEFAULT ''     COMMENT '应用版本',
  push_token     VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '离线推送Token(APNS/FCM)',
  online_status  TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '在线状态 0离线/1在线',
  last_active_at DATETIME        NULL                    COMMENT '最近活跃时间',
  created_at     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '绑定时间',
  updated_at     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_user_device (user_id, device_id),
  KEY idx_online_status (online_status),
  KEY idx_last_active (last_active_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='用户多端登录设备表';

-- ---------------------------------------------------------------
-- 2.3 会话表 sessions (JWT Token + 刷新机制, 定时清理过期)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS sessions;
CREATE TABLE sessions (
  id                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '会话记录ID',
  token               VARCHAR(255)    NOT NULL                COMMENT 'JWT访问Token',
  user_id             BIGINT UNSIGNED NOT NULL                COMMENT '用户ID',
  device_id           VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '登录设备标识',
  refresh_token       VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '刷新Token',
  refresh_expires_at  DATETIME        NULL                    COMMENT '刷新Token过期时间',
  expires_at          DATETIME        NOT NULL                COMMENT '访问Token过期时间',
  status              TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '状态 0失效/1有效',
  ip                  VARCHAR(45)     NOT NULL DEFAULT ''     COMMENT '登录IP',
  user_agent          VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT 'UA信息',
  created_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  updated_at          DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_token (token),
  KEY idx_user_device (user_id, device_id),
  KEY idx_expires_at (expires_at),
  KEY idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='登录会话表(设计稿建议token用前缀索引, 定长存储更省空间)';

-- ---------------------------------------------------------------
-- 2.4 审计日志表 audit_logs (敏感操作留痕, 设计稿: 按月分区保留6个月)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS audit_logs;
CREATE TABLE audit_logs (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '日志ID',
  user_id       BIGINT UNSIGNED NULL                    COMMENT '操作主体用户ID',
  username      VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '操作主体用户名(冗余, 防用户删除后无法溯源)',
  action        VARCHAR(32)     NOT NULL                COMMENT '操作 login/logout/register/update...',
  resource_type VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '资源类型',
  resource_id   VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '资源ID',
  detail        JSON            NULL                    COMMENT '操作详情(JSON)',
  ip            VARCHAR(45)     NOT NULL DEFAULT ''     COMMENT '操作IP',
  user_agent    VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT 'UA信息',
  status        TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '结果 0失败/1成功',
  error_msg     VARCHAR(512)    NOT NULL DEFAULT ''     COMMENT '失败原因',
  created_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '操作时间',
  PRIMARY KEY (id),
  KEY idx_user_id (user_id),
  KEY idx_action (action),
  KEY idx_created_at (created_at),
  KEY idx_user_action_time (user_id, action, created_at) COMMENT '用户行为分析'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='审计日志表';

-- =====================================================================
-- 二、消息库 im_message  (对应设计稿 §3)
--     messages_YYYYMM 按月分表(表名带月份), 本脚本默认建当月表,
--     跨月/归档请按模板另建。
-- =====================================================================
CREATE DATABASE IF NOT EXISTS im_message
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE im_message;

-- ---------------------------------------------------------------
-- 3.1 单聊消息表 messages_YYYYMM (按月分表模板)
--     分片扩展(§5): 路由键 to_user, 表名 messages_YYYYMM_xx,
--     hash(to_user) % 64 定位表。
--     建当月示例表, 后续每月按模板 create table messages_YYYYMM ...
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS messages_202609;
CREATE TABLE messages_202609 (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '自增主键(仅库内排序用)',
  msg_id      BIGINT UNSIGNED NOT NULL                COMMENT '消息ID(雪花算法, 全局唯一)',
  from_user   BIGINT UNSIGNED NOT NULL                COMMENT '发送方用户ID',
  to_user     BIGINT UNSIGNED NOT NULL                COMMENT '接收方用户ID',
  msg_type    TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '消息类型 0文本/1图片/2语音/3视频/4文件/5位置/6表情',
  content     TEXT            NULL                    COMMENT '消息内容(文本或JSON)',
  media_urls  JSON            NULL                    COMMENT '媒体URL列表(JSON)',
  status      TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0未读/1已读/2已撤回/3已删除',
  read_at     DATETIME        NULL                    COMMENT '已读时间',
  recall_at   DATETIME        NULL                    COMMENT '撤回时间',
  created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '发送时间',
  updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_msg_id (msg_id),
  KEY idx_from_user (from_user),
  KEY idx_to_user (to_user),
  KEY idx_created_at (created_at),
  KEY idx_to_user_created (to_user, created_at)              COMMENT '拉取历史消息',
  KEY idx_fm_to_created (from_user, to_user, created_at)     COMMENT '查询两人聊天记录',
  KEY idx_to_status_created (to_user, status, created_at)    COMMENT '查询未读消息'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='单聊消息表(按月分表, 2026-09)';

-- ---------------------------------------------------------------
-- 3.2 群聊消息表 group_messages (设计稿: 群数量相对少, 不分表)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS group_messages;
CREATE TABLE group_messages (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '自增主键',
  msg_id      BIGINT UNSIGNED NOT NULL                COMMENT '消息ID(雪花算法)',
  group_id    BIGINT UNSIGNED NOT NULL                COMMENT '群组ID',
  from_user   BIGINT UNSIGNED NOT NULL                COMMENT '发送者用户ID',
  msg_type    TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '消息类型(同单聊)',
  content     TEXT            NULL                    COMMENT '消息内容(文本或JSON)',
  media_urls  JSON            NULL                    COMMENT '媒体URL列表(JSON)',
  at_users    JSON            NULL                    COMMENT '@的用户ID数组(JSON)',
  status      TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0正常/1已撤回/2已删除',
  recall_at   DATETIME        NULL                    COMMENT '撤回时间',
  created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '发送时间',
  updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_msg_id (msg_id),
  KEY idx_group_id (group_id),
  KEY idx_from_user (from_user),
  KEY idx_created_at (created_at),
  KEY idx_group_created (group_id, created_at) COMMENT '拉取群历史消息'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='群聊消息表';

-- ---------------------------------------------------------------
-- 3.3 离线消息表 offline_messages (设计稿: 按月分区/定期清理7天)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS offline_messages;
CREATE TABLE offline_messages (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '记录ID',
  msg_id      BIGINT UNSIGNED NOT NULL                COMMENT '关联原始消息ID(单聊/群聊)',
  user_id     BIGINT UNSIGNED NOT NULL                COMMENT '接收者用户ID',
  from_user   BIGINT UNSIGNED NOT NULL                COMMENT '发送者用户ID',
  msg_type    TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '消息类型(同单聊)',
  content     TEXT            NULL                    COMMENT '消息内容',
  media_urls  JSON            NULL                    COMMENT '媒体URL列表(JSON)',
  is_group    TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '是否群消息 0单聊/1群聊',
  group_id    BIGINT UNSIGNED NULL                    COMMENT '群ID(is_group=1时有值)',
  status      TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0未拉取/1已拉取',
  created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '落库时间',
  delivered_at DATETIME       NULL                    COMMENT '拉取时间',
  PRIMARY KEY (id),
  KEY idx_user_id (user_id),
  KEY idx_created_at (created_at),
  KEY idx_status (status),
  KEY idx_user_status (user_id, status) COMMENT '拉取离线消息'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='离线消息表';

-- ---------------------------------------------------------------
-- 3.4 消息已读状态表 message_read_status (多端已读同步)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS message_read_status;
CREATE TABLE message_read_status (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '记录ID',
  msg_id      BIGINT UNSIGNED NOT NULL                COMMENT '消息ID',
  user_id     BIGINT UNSIGNED NOT NULL                COMMENT '已读用户ID',
  read_at     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '已读时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_msg_user (msg_id, user_id),
  KEY idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='消息已读状态表';

-- ---------------------------------------------------------------
-- 3.5 未读计数表 unread_counts (红点提示, MQ异步刷新)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS unread_counts;
CREATE TABLE unread_counts (
  id                BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '记录ID',
  user_id           BIGINT UNSIGNED NOT NULL                COMMENT '用户ID',
  target_id         BIGINT UNSIGNED NOT NULL                COMMENT '目标ID(好友ID或群组ID)',
  target_type       TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '目标类型 1单聊/2群聊',
  unread_count      INT UNSIGNED    NOT NULL DEFAULT 0      COMMENT '未读数',
  last_read_msg_id  BIGINT UNSIGNED NULL                    COMMENT '最后已读消息ID',
  updated_at        DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_user_target (user_id, target_id, target_type),
  KEY idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='未读计数表';

-- =====================================================================
-- 三、社交库 im_social  (对应设计稿 §4)
--     分片扩展(§5): friends/blacklist 路由 user_id,
--     groups/group_members 路由 group_id。
-- =====================================================================
CREATE DATABASE IF NOT EXISTS im_social
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE im_social;

-- ---------------------------------------------------------------
-- 4.1 好友关系表 friends
--     分片: hash(user_id)%16 定库, hash(user_id)%64 定表 (1024张)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS friends;
CREATE TABLE friends (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '关系ID',
  user_id     BIGINT UNSIGNED NOT NULL                COMMENT '用户ID',
  friend_id   BIGINT UNSIGNED NOT NULL                COMMENT '好友用户ID',
  status      TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0待确认/1已确认/2已拒绝/3已删除',
  remark      VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '好友备注名',
  extra       JSON            NULL                    COMMENT '扩展信息(JSON)',
  apply_msg   VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '验证申请信息',
  apply_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '申请时间',
  confirm_at  DATETIME        NULL                    COMMENT '确认时间',
  created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_user_friend (user_id, friend_id),
  KEY idx_user_id (user_id),
  KEY idx_friend_id (friend_id),
  KEY idx_status (status),
  KEY idx_user_status_created (user_id, status, created_at) COMMENT '好友列表查询'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='好友关系表(单份模板)';

-- ---------------------------------------------------------------
-- 4.2 黑名单表 blacklist
--     分片: 同 friends (user_id, 1024张)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS blacklist;
CREATE TABLE blacklist (
  id               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '记录ID',
  user_id          BIGINT UNSIGNED NOT NULL                COMMENT '用户ID',
  blocked_user_id  BIGINT UNSIGNED NOT NULL                COMMENT '被屏蔽用户ID',
  reason           VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '屏蔽原因',
  created_at       DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '屏蔽时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_user_blocked (user_id, blocked_user_id),
  KEY idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='黑名单表';

-- ---------------------------------------------------------------
-- 4.3 好友申请表 friend_requests (3天过期自动失效)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS friend_requests;
CREATE TABLE friend_requests (
  id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '申请ID',
  from_user   BIGINT UNSIGNED NOT NULL                COMMENT '申请人',
  to_user     BIGINT UNSIGNED NOT NULL                COMMENT '接收人',
  apply_msg   VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '验证信息',
  status      TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0待处理/1已接受/2已拒绝/3已过期',
  handled_at  DATETIME        NULL                    COMMENT '处理时间',
  handler_id  BIGINT UNSIGNED NULL                    COMMENT '处理人ID',
  expire_at   DATETIME        NOT NULL                COMMENT '过期时间(默认申请+3天)',
  created_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '申请时间',
  updated_at  DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  KEY idx_from_user (from_user),
  KEY idx_to_user (to_user),
  KEY idx_status (status),
  KEY idx_to_status_created (to_user, status, created_at) COMMENT '待处理申请查询'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='好友申请表';

-- ---------------------------------------------------------------
-- 4.4 群组表 groups
--     分片: hash(group_id)%8 定库, hash(group_id)%32 定表 (256张)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS `groups`;
CREATE TABLE `groups` (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '自增主键',
  group_id      BIGINT UNSIGNED NOT NULL                COMMENT '群组ID(雪花算法, 全局唯一)',
  name          VARCHAR(64)     NOT NULL                COMMENT '群名称',
  avatar        VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '群头像',
  notice        TEXT            NULL                    COMMENT '群公告',
  introduction  VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '群简介',
  owner_id      BIGINT UNSIGNED NOT NULL                COMMENT '群主用户ID',
  member_count  INT UNSIGNED    NOT NULL DEFAULT 0      COMMENT '群成员数(冗余)',
  max_members   INT UNSIGNED    NOT NULL DEFAULT 500    COMMENT '群成员上限(默认500)',
  join_verify   TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '入群需验证 0否/1是',
  allow_invite  TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '允许群成员邀请 0否/1是',
  allow_search  TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '允许被搜索 0否/1是',
  status        TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '状态 0已解散/1正常',
  dismissed_at  DATETIME        NULL                    COMMENT '解散时间',
  created_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  updated_at    DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_group_id (group_id),
  KEY idx_owner_id (owner_id),
  KEY idx_name (name),
  KEY idx_status (status),
  KEY idx_status_created (status, created_at) COMMENT '活跃群组查询'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='群组表(单份模板)';

-- ---------------------------------------------------------------
-- 4.5 群成员表 group_members
--     分片: 与 groups 同键同算法 (group_id, 256张), 关联可同库
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS group_members;
CREATE TABLE group_members (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '成员记录ID',
  group_id       BIGINT UNSIGNED NOT NULL                COMMENT '群组ID',
  user_id        BIGINT UNSIGNED NOT NULL                COMMENT '用户ID',
  role           TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '角色 0普通/1管理员/2群主',
  nickname       VARCHAR(64)     NOT NULL DEFAULT ''     COMMENT '群内昵称',
  join_way       TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '加入方式 0邀请/1申请/2直接加入',
  mute_expire_at DATETIME        NULL                    COMMENT '禁言截止时间(NULL=未禁言)',
  status         TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '状态 0已退出/1正常/2被踢出',
  last_read_time DATETIME        NULL                    COMMENT '群内最后已读时间',
  join_at        DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '入群时间',
  leave_at       DATETIME        NULL                    COMMENT '退群时间',
  created_at     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
  updated_at     DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_user_id (user_id),
  KEY idx_role (role),
  KEY idx_status (status),
  KEY idx_group_status (group_id, status) COMMENT '群成员列表查询'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='群成员表';

-- ---------------------------------------------------------------
-- 4.6 群申请表 group_applies (含邀请机制)
-- ---------------------------------------------------------------
DROP TABLE IF EXISTS group_applies;
CREATE TABLE group_applies (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '申请ID',
  group_id     BIGINT UNSIGNED NOT NULL                COMMENT '群组ID',
  applicant_id BIGINT UNSIGNED NOT NULL                COMMENT '申请人用户ID',
  inviter_id   BIGINT UNSIGNED NULL                    COMMENT '邀请人用户ID(邀请加入时)',
  apply_msg    VARCHAR(255)    NOT NULL DEFAULT ''     COMMENT '申请信息',
  status       TINYINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '状态 0待处理/1已通过/2已拒绝/3已过期',
  handler_id   BIGINT UNSIGNED NULL                    COMMENT '处理人用户ID',
  handled_at   DATETIME        NULL                    COMMENT '处理时间',
  expire_at    DATETIME        NOT NULL                COMMENT '过期时间',
  created_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '申请时间',
  updated_at   DATETIME        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新时间',
  PRIMARY KEY (id),
  KEY idx_group_id (group_id),
  KEY idx_applicant_id (applicant_id),
  KEY idx_status (status),
  KEY idx_group_status_created (group_id, status, created_at) COMMENT '群待处理申请查询'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='群申请表';
