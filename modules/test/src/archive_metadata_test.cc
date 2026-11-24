#include "fixture_directory.h"

#include <codec/7z.h>
#include <codec/archive.h>
#include <codec/zip.h>
#include <gtest/gtest.h>
#include <chrono>
#include <c_string_view.h>
#include <filesystem>
#include <fstream>
#include <fs.h>
#include <iostream>
#include <string_cvt.h>

namespace ysm::codec {
namespace {
constexpr size_t kZipDirectoryOffset = 262230;
constexpr size_t kZipEndSize = 22;

class ZipMetadataTest : public testing::Test {
  protected:
    void SetUp() override {
        const auto& directory = test::FixtureDirectory();
        if (directory.empty()) GTEST_SKIP() << "Supply --fixture-dir=<path>";

        const auto fixture_path = directory / "catalog-archives" / "directory.zip";
        BufferManaged fixture;
        const auto status = FileRead(fixture_path, fixture);
        ASSERT_TRUE(status.ok()) << status;
        ASSERT_GT(fixture.size(), kZipDirectoryOffset + kZipEndSize)
            << PathToU8(fixture_path);
        bytes_.assign(reinterpret_cast<const char*>(fixture.data()), fixture.size());

        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ysm-zip-metadata-" + std::to_string(timestamp) + ".zip");
    }

    void TearDown() override {
        if (!path_.empty()) std::filesystem::remove(path_);
    }

    void Write() const {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
        ASSERT_TRUE(output);
    }

    std::filesystem::path path_;
    std::string bytes_;
};

template <typename T>
void CheckDirectory(const std::filesystem::path& directory, const char* name,
                    bool crc, uint64_t metadata_start) {
    const auto path = directory / "catalog-archives" / name;
    SCOPED_TRACE(PathToU8(path));
    ArchiveTrace trace;
    Archive<T> archive(path, &trace);
    ASSERT_TRUE(archive.Ok().ok()) << archive.Ok();
    ASSERT_EQ(archive.Entries().size(), 2);
    EXPECT_EQ(archive.Entries()[0].Size(), 256 * 1024);
    EXPECT_EQ(archive.Entries()[0].Crc().has_value(), crc);
    EXPECT_EQ(archive.Entries()[1].Crc().has_value(), crc);
    EXPECT_EQ(trace.directories, 1);
    EXPECT_EQ(trace.extracts, 0);
    uint64_t total = 0;
    std::cout << "ARCHIVE_TRACE name=" << name << " ranges=";
    for (auto read : trace.reads) {
        EXPECT_TRUE(read.offset >= metadata_start ||
                    (read.offset == 0 && read.size <= 32));
        total += read.size;
        std::cout << read.offset << ':' << read.size << ',';
    }
    std::cout << " bytes=" << total << '\n';
    EXPECT_LT(total, 64 * 1024);
    BufferManaged output;
    auto extracted = archive.Extract("a.bin"_sv, output);
    ASSERT_TRUE(extracted.ok()) << name << ": " << extracted;
    EXPECT_EQ(trace.extracts, 1);
    EXPECT_EQ(output.size(), 256 * 1024);
    ASSERT_TRUE(archive.Extract("b.txt"_sv, output).ok());
    EXPECT_EQ(output.size(), 6);
}

TEST(ArchiveMetadataTest, ZipDirectoryDoesNotExtractMembers) {
    const auto& directory = test::FixtureDirectory();
    if (directory.empty()) GTEST_SKIP() << "Supply --fixture-dir=<path>";

    CheckDirectory<Zip>(directory, "directory.zip", true, kZipDirectoryOffset);
    CheckDirectory<Zip>(directory, "directory-comment.zip", true, kZipDirectoryOffset);
    CheckDirectory<Zip>(directory, "directory-zip64.zip", true, kZipDirectoryOffset);
    CheckDirectory<Zip>(directory, "directory-prefix.zip", true, 263254);
}

TEST_F(ZipMetadataTest, DigitalSignatureMustBeAtTheEndOfTheMemberDirectory) {
    constexpr std::string_view signature("PK\x05\x05\x03\0sig", 9);
    const auto signature_offset = bytes_.size() - kZipEndSize;
    bytes_.insert(signature_offset, signature);
    const auto end = signature_offset + signature.size();
    const auto directory_size = end - kZipDirectoryOffset;
    for (size_t i = 0; i < 4; ++i)
        bytes_[end + 12 + i] = static_cast<char>(directory_size >> (i * 8));
    const BufferViewR buffer(reinterpret_cast<const Byte*>(bytes_.data()),
                             bytes_.size());

    Write();
    {
        ArchiveTrace trace;
        Archive<Zip> file(path_, &trace);
        ASSERT_TRUE(file.Ok().ok()) << file.Ok();
        ASSERT_EQ(file.Entries().size(), 2);
        for (auto read : trace.reads)
            EXPECT_GE(read.offset, kZipDirectoryOffset);
        BufferManaged output;
        ASSERT_TRUE(file.Extract("b.txt"_sv, output).ok());
        EXPECT_EQ(output.size(), 6);
        Archive<Zip> memory(buffer);
        ASSERT_TRUE(memory.Ok().ok()) << memory.Ok();
        EXPECT_EQ(memory.Entries().size(), 2);
    }

    bytes_[signature_offset + 4] = 2;
    Write();
    EXPECT_EQ(Archive<Zip>(path_).Ok().code(), absl::StatusCode::kDataLoss);
    EXPECT_EQ(Archive<Zip>(buffer).Ok().code(), absl::StatusCode::kDataLoss);

    bytes_[signature_offset + 4] = 3;
    bytes_[end + 8] = bytes_[end + 10] = 1;
    Write();
    EXPECT_EQ(Archive<Zip>(path_).Ok().code(), absl::StatusCode::kDataLoss);
    EXPECT_EQ(Archive<Zip>(buffer).Ok().code(), absl::StatusCode::kDataLoss);

    bytes_[end + 8] = bytes_[end + 10] = 2;
    bytes_.replace(signature_offset, 4, "PK\x06\x06");
    Write();
    EXPECT_EQ(Archive<Zip>(path_).Ok().code(), absl::StatusCode::kDataLoss);
}

TEST_F(ZipMetadataTest, CommentScanDoesNotReadBeforeTheEndRecord) {
    const auto size = bytes_.size();
    const auto end = size - kZipEndSize;
    for (const auto length : {1, 19, 20, 21, 65535}) {
        SCOPED_TRACE(length);
        bytes_.resize(size);
        bytes_[size - 2] = static_cast<char>(length);
        bytes_[size - 1] = static_cast<char>(length >> 8);
        bytes_.append(length, 'x');
        Write();

        ArchiveTrace trace;
        Archive<Zip> file(path_, &trace);
        ASSERT_TRUE(file.Ok().ok()) << file.Ok();
        ASSERT_EQ(file.Entries().size(), 2);
        EXPECT_LT(trace.reads.size(), static_cast<size_t>(length / 20 + 10));
        for (auto read : trace.reads) {
            if (read.offset == kZipDirectoryOffset)
                EXPECT_EQ(read.size, end - kZipDirectoryOffset);
            else
                EXPECT_GE(read.offset, end);
        }
    }
}

TEST(ArchiveMetadataTest, SevenZipCompressedAndPlainHeadersPreserveMissingCrc) {
    const auto& directory = test::FixtureDirectory();
    if (directory.empty()) GTEST_SKIP() << "Supply --fixture-dir=<path>";

    CheckDirectory<SevenZip>(directory, "encoded.7z", true, 262198);
    CheckDirectory<SevenZip>(directory, "plain.7z", true, 262198);
    CheckDirectory<SevenZip>(directory, "encoded-no-crc.7z", false, 262198);
    CheckDirectory<SevenZip>(directory, "plain-no-crc.7z", false, 262198);
}
}  // namespace
}  // namespace ysm::codec
