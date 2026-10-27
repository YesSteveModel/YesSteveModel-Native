#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

#include <java/entry.h>

namespace ysm::legacy {
namespace {
TEST(LegacyJniEntryTest, StaticModuleContributesExactMainRegistryEntries) {
    auto entries_or = java::internal::CollectAndValidateEntries(
        java::internal::RegisteredEntries());
    ASSERT_TRUE(entries_or.ok()) << entries_or.status();

    constexpr std::string_view kClassName =
        "com/elfmcys/ysm/natives/legacy/NativeLegacyImporter";
    const auto& entries = entries_or.value();
    const auto has_entry = [&entries](std::string_view class_name,
                                      std::string_view method_name,
                                      std::string_view signature) {
        return std::ranges::any_of(entries, [&](const auto* entry) {
            return entry->class_name == class_name &&
                   entry->method_name == method_name &&
                   entry->signature == signature;
        });
    };

    EXPECT_TRUE(
        has_entry(kClassName, "nImport",
                  "(Ljava/lang/Object;J)Lcom/elfmcys/ysm/natives/legacy/"
                  "NativeLegacyImportResult$Output;"));
    EXPECT_TRUE(has_entry(kClassName, "nShare", "(J)J"));
}
}  // namespace
}  // namespace ysm::legacy
