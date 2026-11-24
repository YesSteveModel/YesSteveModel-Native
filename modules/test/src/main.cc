#include "fixture_directory.h"

#include <gtest/gtest.h>

#include <iostream>
#include <string_view>

#include <log.h>

namespace ysm::test {
namespace {
std::filesystem::path fixture_directory;
}  // namespace

const std::filesystem::path& FixtureDirectory() { return fixture_directory; }
}  // namespace ysm::test

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    constexpr std::wstring_view kFixtureOption = L"--fixture-dir";
#else
int main(int argc, char** argv) {
    constexpr std::string_view kFixtureOption = "--fixture-dir";
#endif
    using ysm::test::fixture_directory;

    ::testing::InitGoogleTest(&argc, argv);
    for (int i = 1; i < argc; ++i) {
        const std::basic_string_view argument(argv[i]);
        const auto separator = argument.find('=');
        if (argument.substr(0, separator) != kFixtureOption) continue;
        if (separator == argument.npos || separator + 1 == argument.size() ||
            !fixture_directory.empty()) {
            std::cerr << "Supply --fixture-dir=<path> once with a non-empty path\n";
            return 1;
        }
        fixture_directory = argument.substr(separator + 1);
    }

    ysm::InitializeLogging();
    auto status =
        ysm::ConfigureLogLevel(static_cast<int>(ysm::LogLevel::kDebug));
    if (!status.ok()) {
        YSM_LOG(ERROR, "Failed to enable native test logging: {}",
                status.message());
        return 1;
    }
    YSM_LOG_DEBUG("Native unit tests started with Debug logging enabled");
    return RUN_ALL_TESTS();
}
