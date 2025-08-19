#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_IPID_LOGGING_IPID_DEPTH_LOGGING_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_IPID_LOGGING_IPID_DEPTH_LOGGING_H_

#include <string>
#include <vector>

#include "third_party/blink/renderer/platform/allow_discouraged_type.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class PLATFORM_EXPORT IpidDepthLog {
 public:
  // 支持传入函数名或标识符的构造函数
  IpidDepthLog(const std::string& function_name);

  ~IpidDepthLog();

  // 返回当前调用栈的格式化字符串
  // 格式为：[#x ($a-$b-$c)]，其中x表示全局初始化次数，
  // 括号内的数字表示调用栈中的各级ID
  std::string StackStr() const;

  // 添加记录的字段
  void AddField(const char* key, const char* value);

  // 添加记录的字段
  void AddField(const char* key, const std::string& value);

  // 添加记录的字段
  void AddField(const char* key, const WTF::String& value);

  // 统一输出上下文信息和所有记录的字段
  void PrintContext(const char* message) const;

  // 禁止拷贝和赋值
  IpidDepthLog(const IpidDepthLog&) = delete;
  IpidDepthLog& operator=(const IpidDepthLog&) = delete;

 private:
  // 记录当前实例的初始化ID
  int instance_id_;

  // 记录函数名或标识符
  std::string function_name_;

  // 记录字段的键值对，使用vector保持插入顺序
  std::vector<std::pair<std::string, std::string>> fields_
      ALLOW_DISCOURAGED_TYPE("Debug code");

  // 模仿 JavaScript 中的 `debugger` 关键字，用于调试
  void Debugger() const;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_IPID_LOGGING_IPID_DEPTH_LOGGING_H_
