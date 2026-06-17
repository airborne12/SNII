#pragma once

#include <string>
#include <utility>

namespace snii {

enum class StatusCode {
  kOk,
  kCorruption,
  kNotFound,
  kInvalidArgument,
  kIoError,
  kUnsupported,
  kInternal,
};

// 轻量错误类型：成功为 kOk 且无消息；失败携带 code + 人类可读消息。
// 跨 API 边界一律返回 Status，禁止静默失败。
class Status {
 public:
  Status() = default;

  static Status OK() { return Status(); }
  static Status Corruption(std::string m) { return Status(StatusCode::kCorruption, std::move(m)); }
  static Status NotFound(std::string m) { return Status(StatusCode::kNotFound, std::move(m)); }
  static Status InvalidArgument(std::string m) { return Status(StatusCode::kInvalidArgument, std::move(m)); }
  static Status IoError(std::string m) { return Status(StatusCode::kIoError, std::move(m)); }
  static Status Unsupported(std::string m) { return Status(StatusCode::kUnsupported, std::move(m)); }
  static Status Internal(std::string m) { return Status(StatusCode::kInternal, std::move(m)); }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string to_string() const;

 private:
  Status(StatusCode c, std::string m) : code_(c), message_(std::move(m)) {}

  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

}  // namespace snii

// 对返回 Status 的表达式短路返回（错误向上传播）。
#define SNII_RETURN_IF_ERROR(expr)    \
  do {                                \
    ::snii::Status _s = (expr);       \
    if (!_s.ok()) return _s;          \
  } while (0)
