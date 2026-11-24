#pragma once

#include <filesystem>

namespace ysm::test {
// An empty path means external fixtures were not requested.
const std::filesystem::path& FixtureDirectory();
}  // namespace ysm::test
