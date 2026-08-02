#pragma once

#include "ytec/clonecore/result.h"

#include <functional>
#include <string>
#include <string_view>

namespace ytec::clonecore {

enum class LogLevel {
  info,
  warning,
  error,
  debug,
};

struct LogRecord final {
  LogLevel level{LogLevel::info};
  std::wstring message;
};

class Logger final {
 public:
  using Sink = std::function<void(const LogRecord&)>;

  explicit Logger(Sink sink);

  void write(LogLevel level, std::wstring_view message) const noexcept;
  void info(std::wstring_view message) const noexcept;
  void warning(std::wstring_view message) const noexcept;
  void error(std::wstring_view message) const noexcept;
  void debug(std::wstring_view message) const noexcept;

 private:
  Sink sink_;
};

[[nodiscard]] Logger make_stderr_logger();
// Creates a new UTF-8 BOM log file without overwriting an existing file.
// Records are single-line sanitized and readable while the application runs.
[[nodiscard]] Result<Logger> make_utf8_file_logger(
    const std::wstring& path,
    bool debug_enabled);
[[nodiscard]] std::wstring_view log_level_name(LogLevel level) noexcept;

}  // namespace ytec::clonecore
