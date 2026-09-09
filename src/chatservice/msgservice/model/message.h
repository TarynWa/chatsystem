// 消息域纯结构体契约(dao/service/impl 层间用, 不引 protobuf)
#pragma once
#include <cstdint>
#include <string>

namespace chatservice::model {

// 单条单聊消息(历史/拉新共用)
struct MessageRow {
  uint64_t msg_id = 0;
  uint64_t from_user = 0;
  uint64_t to_user = 0;
  int msg_type = 0;  // 0文本/1图片/2语音/3视频/4文件/5位置/6表情
  std::string content;
  std::string media_urls;  // JSON 数组串
  std::string created_at;  // "YYYY-MM-DD HH:MM:SS"
};

// 发送回显
struct SentMessage {
  uint64_t msg_id = 0;
  std::string created_at;
};

} // namespace chatservice::model
