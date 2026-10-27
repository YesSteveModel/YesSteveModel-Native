#include <array>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <absl/base/log_severity.h>
#include <absl/log/globals.h>
#include <gtest/gtest.h>

#include <log.h>

namespace ysm {
namespace {
static_assert(std::same_as<decltype(LoggingScope(std::declval<std::string>())),
                           LoggingScope<std::string>>);
static_assert(std::same_as<decltype(LoggingScope(std::string_view{})),
                           LoggingScope<std::string_view>>);
static_assert(std::same_as<decltype(LoggingScope("literal")),
                           LoggingScope<std::string_view>>);
static_assert(std::same_as<decltype(LoggingScope(std::declval<char*>())),
                           LoggingScope<std::string_view>>);
static_assert(std::same_as<decltype(LoggingScope(std::declval<const char*>())),
                           LoggingScope<std::string_view>>);
static_assert(sizeof(LoggingScope<std::string_view>) <
              sizeof(LoggingScope<std::string>));
static_assert(!std::is_copy_constructible_v<LoggingScope<std::string_view>>);
static_assert(!std::is_move_constructible_v<LoggingScope<std::string>>);

struct LogLevelExpectation {
    LogLevel level;
    absl::LogSeverityAtLeast severity;
    bool debug_enabled;
    bool trace_enabled;
};

TEST(LogTest, MapsSupportedLevelsToAbseilConfiguration) {
    constexpr std::array kExpectations{
        LogLevelExpectation{LogLevel::kAll,
                            absl::LogSeverityAtLeast::kInfo, true, true},
        LogLevelExpectation{LogLevel::kTrace,
                            absl::LogSeverityAtLeast::kInfo, true, true},
        LogLevelExpectation{LogLevel::kDebug,
                            absl::LogSeverityAtLeast::kInfo, true, false},
        LogLevelExpectation{LogLevel::kInfo,
                            absl::LogSeverityAtLeast::kInfo, false, false},
        LogLevelExpectation{LogLevel::kWarning,
                            absl::LogSeverityAtLeast::kWarning, false, false},
        LogLevelExpectation{LogLevel::kError,
                            absl::LogSeverityAtLeast::kError, false, false},
        LogLevelExpectation{LogLevel::kFatal,
                            absl::LogSeverityAtLeast::kFatal, false, false},
        LogLevelExpectation{LogLevel::kOff,
                            absl::LogSeverityAtLeast::kInfinity, false, false},
    };

    for (const auto& expectation : kExpectations) {
        ASSERT_TRUE(
            ConfigureLogLevel(static_cast<int>(expectation.level)).ok());
        EXPECT_EQ(absl::MinLogLevel(), expectation.severity);
        EXPECT_EQ(absl::StderrThreshold(), expectation.severity);
        EXPECT_EQ(YSM_LOG_DEBUG_ENABLED(), expectation.debug_enabled);
        EXPECT_EQ(YSM_LOG_TRACE_ENABLED(), expectation.trace_enabled);
    }
}

TEST(LogTest, RejectsInvalidLevelWithoutChangingConfiguration) {
    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kDebug)).ok());

    auto status = ConfigureLogLevel(100);

    EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(absl::MinLogLevel(), absl::LogSeverityAtLeast::kInfo);
    EXPECT_TRUE(YSM_LOG_DEBUG_ENABLED());
}

TEST(LogTest, DoesNotEvaluateDisabledDebugArguments) {
    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kInfo)).ok());
    int value = 0;

    YSM_LOG_DEBUG("Side effect: {}", ++value);

    EXPECT_EQ(value, 0);
}

TEST(LogTest, OwnsStringNames) {
    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kInfo)).ok());
#if GTEST_HAS_STREAM_REDIRECTION
    testing::internal::CaptureStdout();
#endif
    std::string name(80, 'x');
    {
        LoggingScope scope(name, true);
        name.assign(80, 'y');
        YSM_LOG(INFO, "copied name");
    }
    {
        LoggingScope scope(std::string(80, 'z'), true);
        YSM_LOG(INFO, "temporary name");
    }
#if GTEST_HAS_STREAM_REDIRECTION
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("[" + std::string(80, 'x') + "] copied name"),
              std::string::npos);
    EXPECT_NE(output.find("[" + std::string(80, 'z') + "] temporary name"),
              std::string::npos);
#endif
}

TEST(LogTest, BorrowsStringViewsAndCharacterPointers) {
    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kInfo)).ok());
#if GTEST_HAS_STREAM_REDIRECTION
    testing::internal::CaptureStdout();
#endif
    char name[] = "view-tail";
    {
        LoggingScope scope(std::string_view(name, 4), true);
        name[0] = 'V';
        YSM_LOG(INFO, "borrowed view");
    }
    {
        LoggingScope scope(name, true);
        name[0] = 'C';
        YSM_LOG(INFO, "borrowed pointer");
    }
#if GTEST_HAS_STREAM_REDIRECTION
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("[View] borrowed view"), std::string::npos);
    EXPECT_NE(output.find("[Ciew-tail] borrowed pointer"), std::string::npos);
#endif
}

TEST(LogTest, RestoresOuterScopeAfterExceptionUnwinding) {
    ASSERT_TRUE(ConfigureLogLevel(static_cast<int>(LogLevel::kInfo)).ok());
#if GTEST_HAS_STREAM_REDIRECTION
    testing::internal::CaptureStdout();
#endif
    {
        LoggingScope outer("outer", true);
        EXPECT_THROW(
            {
                LoggingScope inner(std::string(80, 'x'), true);
                throw std::runtime_error("unwind logging scope");
            },
            std::runtime_error);
        YSM_LOG(INFO, "outer log");
    }
    YSM_LOG(INFO, "unscoped log");
#if GTEST_HAS_STREAM_REDIRECTION
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("[outer] outer log"), std::string::npos);
    EXPECT_NE(output.find("unscoped log"), std::string::npos);
    EXPECT_EQ(output.find("[outer] unscoped log"), std::string::npos);
#endif
}
}  // namespace
}  // namespace ysm
