#pragma once

#include <concepts>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <absl/log/absl_log.h>
#include <absl/log/log_sink.h>
#include <absl/status/status.h>

#define YSM_LOG(severity, ...) \
    ABSL_LOG(severity) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)
#define YSM_PLOG(severity, ...) \
    ABSL_PLOG(severity) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)

#define YSM_LOG_IF(severity, condition, ...) \
    ABSL_LOG_IF(severity, condition) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)

#define YSM_PLOG_IF(severity, condition, ...) \
    ABSL_PLOG_IF(severity, condition) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)

#ifndef YSM_ENABLE_DEBUG_LOG
#ifdef YSM_DEBUG
#define YSM_ENABLE_DEBUG_LOG 1
#else
#define YSM_ENABLE_DEBUG_LOG 0
#endif
#endif

#if YSM_ENABLE_DEBUG_LOG
#define YSM_LOG_DEBUG(...) \
    ABSL_VLOG(1) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)
#define YSM_LOG_TRACE(...) \
    ABSL_VLOG(2) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)
#define YSM_LOG_DEBUG_IF(condition, ...) \
    ABSL_VLOG_IF(1, condition) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)
#define YSM_LOG_DEBUG_EVERY_N_SEC(n_seconds, ...) \
    ABSL_VLOG_EVERY_N_SEC(1, n_seconds) \
        .ToSinkOnly(::ysm::log_internal::GetLogSink()) \
        << std::format(__VA_ARGS__)
#define YSM_LOG_DEBUG_ENABLED() ABSL_VLOG_IS_ON(1)
#define YSM_LOG_TRACE_ENABLED() ABSL_VLOG_IS_ON(2)
#else
#define YSM_LOG_DEBUG(...) \
    static_cast<void>(sizeof(std::format(__VA_ARGS__)))
#define YSM_LOG_TRACE(...) \
    static_cast<void>(sizeof(std::format(__VA_ARGS__)))
#define YSM_LOG_DEBUG_IF(condition, ...)       \
    do {                                       \
        static_cast<void>(sizeof(condition));  \
        YSM_LOG_DEBUG(__VA_ARGS__);            \
    } while (false)
#define YSM_LOG_DEBUG_EVERY_N_SEC(n_seconds, ...) \
    do {                                          \
        static_cast<void>(sizeof(n_seconds));     \
        YSM_LOG_DEBUG(__VA_ARGS__);               \
    } while (false)
#define YSM_LOG_DEBUG_ENABLED() false
#define YSM_LOG_TRACE_ENABLED() false
#endif

namespace ysm {
namespace log_internal {
absl::LogSink* GetLogSink();
std::size_t PushLoggingScope(std::string_view name, bool no_source);
void PopLoggingScope(std::size_t depth) noexcept;
}

template <typename Name>
    requires(std::same_as<Name, std::string> ||
             std::same_as<Name, std::string_view>)
class [[nodiscard]] LoggingScope final {
public:
    explicit LoggingScope(Name name, bool no_source = false)
        : name_(std::move(name)),
          depth_(log_internal::PushLoggingScope(name_, no_source)) {}

    ~LoggingScope() noexcept {
        log_internal::PopLoggingScope(depth_);
    }

    LoggingScope(const LoggingScope&) = delete;
    LoggingScope& operator=(const LoggingScope&) = delete;
    LoggingScope(LoggingScope&&) = delete;
    LoggingScope& operator=(LoggingScope&&) = delete;

private:
    Name name_;
    std::size_t depth_;
};

LoggingScope(const char*, bool = false) -> LoggingScope<std::string_view>;

enum class LogLevel : int {
    kAll,
    kTrace,
    kDebug,
    kInfo,
    kWarning,
    kError,
    kFatal,
    kOff,
};

void InitializeLogging();
absl::Status ConfigureLogLevel(int level);
std::string_view LogLevelName(LogLevel level);
}  // namespace ysm
