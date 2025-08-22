#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_IPID_LOGGING_IPID_DEPTH_LOGGING_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_IPID_LOGGING_IPID_DEPTH_LOGGING_H_

#include <concepts>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "third_party/blink/renderer/platform/allow_discouraged_type.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

namespace {

// 检测类型是否具有 ToString() 方法
template <typename T>
concept HasToString = requires(T&& t) {
  { t.ToString() } -> std::convertible_to<WTF::String>;
};

// 检测类型是否支持 operator<<
template <typename T>
concept StreamOutputable =
    requires(std::ostringstream& os, const T& t) { os << t; };

/**
  智能类型转换器，能够自动识别：
  1. WTF::String 类型
  2. 具有 ToString() 方法的类型
  3. 支持 operator<< 的类型
  并将它们都转换为 std::string
*/
template <typename T>
using format_type_converter_t =
    std::conditional_t<std::is_same_v<std::remove_cvref_t<T>, WTF::String> ||
                           HasToString<std::remove_cvref_t<T>> ||
                           StreamOutputable<std::remove_cvref_t<T>>,
                       std::string,
                       T>;

/**
  智能值转换函数，能够自动处理：
  1. WTF::String → std::string (通过 .Utf8())
  2. 具有 ToString() 方法的对象 → std::string (通过 .ToString().Utf8())
  3. 支持 operator<< 的对象 → std::string (通过 ostringstream)
  4. 其他类型保持不变
*/
template <typename T>
decltype(auto) ConvertWtfStringToStdString(T&& arg) {
  using DecayedT = std::remove_cvref_t<T>;

  if constexpr (std::is_same_v<DecayedT, WTF::String>) {
    return arg.Utf8();
  } else if constexpr (HasToString<T>) {
    return arg.ToString().Utf8();
  } else if constexpr (StreamOutputable<T>) {
    std::ostringstream oss;
    oss << arg;
    return oss.str();
  } else {
    return std::forward<T>(arg);
  }
}

}  // namespace

// 全局日志开关控制函数
PLATFORM_EXPORT void SetIpidLoggingEnabled(bool enabled);
PLATFORM_EXPORT bool IsIpidLoggingEnabled();

class PLATFORM_EXPORT IpidDepthLog {
 public:
  // 支持传入函数名或标识符的构造函数
  explicit IpidDepthLog(const std::string& function_name);

  ~IpidDepthLog();

  // 返回当前调用栈的格式化字符串
  // 格式为：[#x ($a-$b-$c)]，其中x表示全局初始化次数，
  // 括号内的数字表示调用栈中的各级ID
  std::string StackStr() const;

  // 打印日志。使用 std::format 语法，并且额外支持 WTF::String 和具有 ToString()
  // 方法的对象；日志会自动使用本类的语法来打印。
  template <typename... Args>
  void FPrint(std::format_string<format_type_converter_t<Args>...> fmt,
              Args&&... args) {
    std::string formatted = std::format(
        fmt, ConvertWtfStringToStdString(std::forward<Args>(args))...);
    FPrintInternal(formatted);
  }

  // 禁止拷贝和赋值
  IpidDepthLog(const IpidDepthLog&) = delete;
  IpidDepthLog& operator=(const IpidDepthLog&) = delete;

 private:
  // 记录当前实例的初始化ID
  int instance_id_;

  // 记录函数名或标识符
  std::string function_name_;

  // 记录当前类是否至少打印过一次 stacktrace
  bool has_printed_stacktrace_ = false;

  // FPrint 的内部逻辑
  void FPrintInternal(const std::string_view& formatted);

  // 模仿 JavaScript 中的 `debugger` 关键字，用于调试
  void Debugger() const;

  // 打印日志通用逻辑，如添加前后缀等
  void PrintMessage(const char* message);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_IPID_LOGGING_IPID_DEPTH_LOGGING_H_
