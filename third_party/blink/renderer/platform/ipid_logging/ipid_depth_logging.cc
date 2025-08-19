#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"

#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"

#include <iostream>
#include <sstream>
#include <vector>

namespace blink {

namespace {

// 记录全局初始化计数
thread_local int g_init_count = 0;
// 记录调用栈
thread_local std::vector<int> g_call_stack;
// 防止某些输出结果被优化
volatile int g_blackhole = 0;

}  // namespace

IpidDepthLog::IpidDepthLog(const std::string& function_name)
    : function_name_(function_name) {
  ++g_init_count;
  instance_id_ = g_init_count;
  g_call_stack.push_back(instance_id_);
}

IpidDepthLog::~IpidDepthLog() {
  // 防止析构出问题
  if (!g_call_stack.empty()) {
    g_call_stack.pop_back();
  }
}

std::string IpidDepthLog::StackStr() const {
  std::ostringstream oss;

  oss << "[#" << instance_id_ << " (";

  // 按照格式打印调用栈
  for (size_t i = 0; i < g_call_stack.size(); ++i) {
    if (i > 0) {
      oss << "-";
    }
    oss << "$" << g_call_stack[i];
  }

  oss << ")]";

  return oss.str();
}

void IpidDepthLog::AddField(const char* key, const char* value) {
  fields_.emplace_back(key, value);
}

void IpidDepthLog::AddField(const char* key, const std::string& value) {
  fields_.emplace_back(key, value);
}

void IpidDepthLog::AddField(const char* key, const WTF::String& value) {
  fields_.emplace_back(key, value.Utf8());
}

void IpidDepthLog::PrintContext(const char* message) const {
  std::cout << "[ipid][" << function_name_ << "]" << StackStr() << " "
            << message << "\n";

  // 输出所有记录的字段
  for (const auto& field : fields_) {
    bool do_wrap = field.second.find("\n") != std::string::npos ||
                   field.second.size() >= 80;
    std::cout << "  [[" << field.first << "]]" << (do_wrap ? ":\n" : ": ")
              << field.second << "\n";
  }

  std::cout << std::endl;

  Debugger();
}

void IpidDepthLog::Debugger() const {
  int curr_instance_id = instance_id_;
  int curr_init_count = g_init_count;
  int curr_call_stack_size = g_call_stack.size();

  // 可以断在这条语句上
  g_blackhole = curr_instance_id + curr_init_count + curr_call_stack_size;
}

}  // namespace blink
