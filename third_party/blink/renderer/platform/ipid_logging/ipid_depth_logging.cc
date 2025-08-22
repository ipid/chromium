
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"

#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"

#include <atomic>
#include <iostream>
#include <sstream>
#include <vector>

#include "base/debug/stack_trace.h"
#include "base/environment.h"

namespace blink {

namespace {

// 打印栈深度时，打多深
constexpr size_t STACK_TRACE_DEPTH = 20;

// 全局日志开关，支持跨线程访问，默认关闭
std::atomic<bool> g_logging_enabled{false};

// 记录全局初始化计数
thread_local int g_init_count = 0;
// 记录输出的日志数，用来给每一条日志分配一个序号
thread_local int g_log_index = 0;
// 记录调用栈
thread_local std::vector<int> g_call_stack;
// 防止某些输出结果被优化
volatile int g_blackhole = 0;

enum StackTraceRequired {
  NotInitialized,
  Required,
  NotRequired,
};

thread_local StackTraceRequired g_stacktrace_required =
    StackTraceRequired::NotInitialized;

}  // namespace

// 设置日志开关。只有当设为 true 时 FPrint 才有效。
void SetIpidLoggingEnabled(bool enabled) {
  // 使用默认的 memory_order_seq_cst 确保所有线程都能及时看到变化
  g_logging_enabled.store(enabled);
}

// 确认当前日志开关是否开启
bool IsIpidLoggingEnabled() {
  return g_logging_enabled.load();
}

IpidDepthLog::IpidDepthLog(const std::string& function_name)
    : function_name_(function_name) {
  ++g_init_count;
  instance_id_ = g_init_count;
  g_call_stack.push_back(instance_id_);

  if (g_stacktrace_required == StackTraceRequired::NotInitialized) {
    auto env = base::Environment::Create();
    g_stacktrace_required = env->HasVar("IPID_LOG_STACKTRACE")
                                ? StackTraceRequired::Required
                                : StackTraceRequired::NotRequired;
  }
}

IpidDepthLog::~IpidDepthLog() {
  // 这里判断一下 empty() 是为了防止出问题，实际上应该不可能不为空
  if (!g_call_stack.empty()) [[likely]] {
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

void IpidDepthLog::PrintMessage(const char* message) {
  // 检查全局日志开关，如果关闭则不执行日志打印、也不自增日志序号
  if (!IsIpidLoggingEnabled()) {
    return;
  }

  g_log_index++;

  std::string stacktrace;

  if (!has_printed_stacktrace_ &&
      g_stacktrace_required == StackTraceRequired::Required) {
    stacktrace = base::debug::StackTrace(STACK_TRACE_DEPTH).ToString();
    has_printed_stacktrace_ = true;
  }

  // message 前的部分保证不换行，但 message 本身可以换行
  std::cout << std::format(
      "[ipid&#LOG!{}][{}]{}[&#msg!{}]{}[/&#msg{}][&#stacktrace!{}]{}[/"
      "&#stacktrace{}][/ipid&#LOG!{}]\n\n",
      g_log_index, function_name_, StackStr(), g_log_index, message,
      g_log_index, g_log_index, stacktrace, g_log_index, g_log_index);
}

void IpidDepthLog::FPrintInternal(const std::string_view& formatted) {
  PrintMessage(formatted.data());
}

void IpidDepthLog::Debugger() const {
  int curr_instance_id = instance_id_;
  int curr_init_count = g_init_count;
  int curr_call_stack_size = g_call_stack.size();
  int curr_log_index = g_log_index;

  // 可以断在这条语句上
  g_blackhole = curr_instance_id + curr_init_count + curr_call_stack_size +
                curr_log_index;
}

}  // namespace blink
