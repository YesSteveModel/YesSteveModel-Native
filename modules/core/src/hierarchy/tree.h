#pragma once

#include <absl/container/flat_hash_map.h>
#include <optional_ref.hpp>
#include <string_view>
#include <type_traits>

#include "non_copyable.h"
#include "path_walker.h"

namespace ysm::hierarchy {
template <typename ValueType>
class Tree : public NonCopyable {
    static_assert(!std::is_reference_v<ValueType>);

   public:
    struct Node;

    gch::optional_ref<ValueType> operator[](
        std::string_view value_path) noexcept {
        return root_[value_path];
    }

    gch::optional_ref<Node> GetChild(std::string_view node_path) noexcept {
        return root_.GetChild(node_path);
    }

    bool emplace(std::string_view path, ValueType& value);

    Node& root() noexcept { return root_; }

   private:
    Node root_;
};

template <typename ValueType>
struct Tree<ValueType>::Node {
    absl::flat_hash_map<std::string_view, Node> children{};
    absl::flat_hash_map<std::string_view, std::reference_wrapper<ValueType>>
        values{};

    gch::optional_ref<ValueType> operator[](
        std::string_view value_path) noexcept;
    gch::optional_ref<Node> GetChild(std::string_view node_path) noexcept;

    friend class Tree;
};

template <typename ValueType>
bool Tree<ValueType>::emplace(std::string_view path, ValueType& value) {
    const PathWalker walker(path);
    if (walker.path.empty()) {
        return false;
    }

    Node* node = &root_;
    auto iter = walker.begin();
    auto last_path_segment = *iter;
    while (++iter != walker.end()) {
        if (node->values.contains(last_path_segment)) [[unlikely]] {
            return false;
        }
        auto node_iter = node->children.find(last_path_segment);
        if (node_iter == node->children.end()) {
            node = &node->children.emplace(last_path_segment, Node{})
                        .first->second;
        } else {
            node = &node_iter->second;
        }
        last_path_segment = *iter;
    }
    if (node->children.contains(last_path_segment)) {
        return false;
    }
    return node->values.try_emplace(last_path_segment, std::ref(value)).second;
}

template <typename ValueType>
gch::optional_ref<ValueType> Tree<ValueType>::Node::operator[](
    std::string_view path) noexcept {
    gch::optional_ref<ValueType> result;

    const PathWalker walker(path);
    if (walker.path.empty()) {
        return result;
    }

    Node* node = this;
    auto iter = walker.begin();
    auto last_path = *iter;
    while (++iter != walker.end()) {
        auto node_iter = node->children.find(last_path);
        if (node_iter == node->children.end()) {
            return result;
        }
        node = &node_iter->second;
        last_path = *iter;
    }
    if (auto value_iter = node->values.find(last_path);
        value_iter != node->values.end()) {
        result.emplace(value_iter->second.get());
    }
    return result;
}

template <typename ValueType>
gch::optional_ref<typename Tree<ValueType>::Node>
Tree<ValueType>::Node::GetChild(std::string_view path) noexcept {
    gch::optional_ref<Node> result;

    const PathWalker walker(path);
    if (walker.path.empty()) {
        return result;
    }

    auto* node = this;
    for (auto seg : walker) {
        auto node_iter = node->children.find(seg);
        if (node_iter == node->children.end()) {
            return result;
        }
        node = &node_iter->second;
    }

    result.emplace(*node);
    return result;
}
}  // namespace ysm::hierarchy
