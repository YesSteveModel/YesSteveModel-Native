#pragma once

#include "hierarchy/tree.h"

#include "buffer.h"
#include "err.h"
#include "fs.h"

namespace ysm::codec {
template <typename ArchiveType, typename EntryType = ArchiveType::EntryType>
class Archive final : NonCopyable {
    using TreeType = hierarchy::Tree<EntryType>;
    using NodeType = TreeType::Node;

    ArchiveType archive_;
    std::vector<EntryType> entries_;
    TreeType tree_;
    absl::Status status_;
    NodeType& root_;

   public:
    using EntryList = decltype(std::views::keys(root_.values));

    static constexpr std::string_view kRoot{};

    explicit Archive(BufferViewR archive_buffer)
        : archive_(archive_buffer), root_(BuildTree()) {}

    template <typename... Args>
    explicit Archive(const fs::path& path, Args&&... args)
        : archive_(path, std::forward<Args>(args)...), root_(BuildTree()) {}

    absl::Status Ok() const { return status_; }

    // The original member set is independent of the VFS root folding.
    std::span<const EntryType> Entries() const { return entries_; }

    auto Files(std::string_view path = kRoot) const {
        static decltype(root_.values) empty;
        if (path.empty()) {
            return std::views::keys(root_.values);
        }
        if (auto node = root_.GetChild(path)) {
            return std::views::keys(node.value().values);
        }
        return std::views::keys(empty);
    }

    auto Directories(std::string_view path = kRoot) const {
        static decltype(root_.children) empty;
        if (path.empty()) {
            return std::views::keys(root_.children);
        }
        if (auto node = root_.GetChild(path)) {
            return std::views::keys(node.value().children);
        }
        return std::views::keys(empty);
    }

    gch::optional_ref<const EntryType> operator[](
        std::string_view file_name) const {
        return root_[file_name];
    }

    absl::Status Extract(const EntryType& entry, BufferManaged& buffer) {
        return archive_.Extract(entry, buffer);
    }

    absl::Status Extract(std::string_view file_name, BufferManaged& buffer) {
        if (auto entry = root_[file_name]) {
            buffer.resize(entry->Size());
            return archive_.Extract(entry.value(), buffer);
        }
        return absl::NotFoundError("File not found");
    }

   private:
    NodeType& BuildTree() {
        status_ = archive_.Visit(
            [&](auto&& entry) { entries_.emplace_back(std::move(entry)); });
        if (!status_.ok())
            return tree_.root();
        for (auto& entry : entries_) {
            if (!tree_.emplace(entry.FullName(), entry)) {
                status_ =
                    absl::InvalidArgumentError("Ambiguous archive member"sv);
                return tree_.root();
            }
        }
        auto root = &tree_.root();
        while (root->values.empty() && root->children.size() == 1) {
            root = &root->children.begin()->second;
        }
        return *root;
    }
};
}  // namespace ysm::codec
