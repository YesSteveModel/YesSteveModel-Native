#include "log.h"

#include <cstdlib>
#include <cstdio>
#include <iterator>

#include <absl/base/log_severity.h>
#include <absl/container/inlined_vector.h>
#include <absl/log/globals.h>
#include <absl/log/initialize.h>
#include <absl/log/log_entry.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "system_allocator.h"

namespace ysm {
using namespace std::string_view_literals;

namespace log_internal {
namespace {
struct LoggingThreadContext {
    struct Scope {
        std::string_view name;
        bool no_source;
    };
    absl::InlinedVector<Scope, 5, SystemAllocator<Scope>> scopes;
    std::basic_string<char, std::char_traits<char>, SystemAllocator<char>>
        message_buffer;
};

LoggingThreadContext& GetLoggingThreadContext() {
    thread_local LoggingThreadContext context;
    return context;
}

std::string_view LogEntryLevelName(const absl::LogEntry& entry) {
    if (entry.verbosity() >= 2) {
        return "TRACE"sv;
    }
    if (entry.verbosity() == 1) {
        return "DEBUG"sv;
    }
    switch (entry.log_severity()) {
        case absl::LogSeverity::kInfo:
            return "INFO"sv;
        case absl::LogSeverity::kWarning:
            return "WARN"sv;
        case absl::LogSeverity::kError:
            return "ERROR"sv;
        case absl::LogSeverity::kFatal:
            return "FATAL"sv;
    }
    return "UNKNOWN"sv;
}

class YsmLogSink final : public absl::LogSink {
public:
    void Send(const absl::LogEntry& entry) override {
        auto* stream = entry.log_severity() >= absl::LogSeverity::kWarning
                           ? stderr
                           : stdout;
        if (entry.stacktrace().empty()) {
            auto& context = GetLoggingThreadContext();
            auto& message = context.message_buffer;
            message.clear();
            std::format_to(
                std::back_inserter(message),
                "[{}] [YSM Native/{}] [",
                absl::FormatTime("%H:%M:%S"sv, entry.timestamp(),
                                 absl::LocalTimeZone()),
                LogEntryLevelName(entry));
            for (auto scope : context.scopes) {
                message.append(scope.name.data(), scope.name.size());
                message.push_back('/');
            }
            if (!context.scopes.empty() && context.scopes[context.scopes.size() - 1].no_source) {
                message[message.size() - 1] = ']';
                message.append(" "sv);
            } else {
                std::format_to(std::back_inserter(message), "{}:{}]: ",
                               entry.source_basename(), entry.source_line());
            }

            auto text = entry.text_message();
            message.append(text.data(), text.size());
            message.push_back('\n');
            std::fwrite(message.data(), 1, message.size(), stream);
        } else {
            auto stacktrace = entry.stacktrace();
            std::fwrite(stacktrace.data(), 1, stacktrace.size(), stream);
        }
        std::fflush(stream);
    }
};

}  // namespace

std::size_t PushLoggingScope(std::string_view name, bool no_source) {
    auto& scopes = GetLoggingThreadContext().scopes;
    auto depth = scopes.size();
    scopes.push_back(LoggingThreadContext::Scope{name, no_source});
    return depth;
}

void PopLoggingScope(std::size_t depth) noexcept {
    auto& scopes = GetLoggingThreadContext().scopes;
    if (scopes.size() != depth + 1) [[unlikely]] {
        std::abort();
    }
    scopes.pop_back();
}

absl::LogSink* GetLogSink() {
    static YsmLogSink sink;
    return &sink;
}
}  // namespace log_internal

namespace {
struct AbslLogConfig {
    absl::LogSeverityAtLeast severity;
    int verbosity;
};

absl::StatusOr<AbslLogConfig> ToAbslLogConfig(int value) {
    switch (static_cast<LogLevel>(value)) {
        case LogLevel::kAll:
        case LogLevel::kTrace:
            return AbslLogConfig{absl::LogSeverityAtLeast::kInfo, 2};
        case LogLevel::kDebug:
            return AbslLogConfig{absl::LogSeverityAtLeast::kInfo, 1};
        case LogLevel::kInfo:
            return AbslLogConfig{absl::LogSeverityAtLeast::kInfo, 0};
        case LogLevel::kWarning:
            return AbslLogConfig{absl::LogSeverityAtLeast::kWarning, -1};
        case LogLevel::kError:
            return AbslLogConfig{absl::LogSeverityAtLeast::kError, -1};
        case LogLevel::kFatal:
            return AbslLogConfig{absl::LogSeverityAtLeast::kFatal, -1};
        case LogLevel::kOff:
            return AbslLogConfig{absl::LogSeverityAtLeast::kInfinity, -1};
    }
    return absl::InvalidArgumentError(
        std::format("Invalid native log level: {}", value));
}
}  // namespace

void InitializeLogging() {
    absl::InitializeLog();
}

absl::Status ConfigureLogLevel(int level) {
    auto config = ToAbslLogConfig(level);
    if (!config.ok()) {
        return config.status();
    }
    absl::SetGlobalVLogLevel(config->verbosity);
    absl::SetMinLogLevel(config->severity);
    absl::SetStderrThreshold(config->severity);
    return absl::OkStatus();
}

std::string_view LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kAll:
            return "ALL"sv;
        case LogLevel::kTrace:
            return "TRACE"sv;
        case LogLevel::kDebug:
            return "DEBUG"sv;
        case LogLevel::kInfo:
            return "INFO"sv;
        case LogLevel::kWarning:
            return "WARN"sv;
        case LogLevel::kError:
            return "ERROR"sv;
        case LogLevel::kFatal:
            return "FATAL"sv;
        case LogLevel::kOff:
            return "OFF"sv;
    }
    return "UNKNOWN"sv;
}
}  // namespace ysm
