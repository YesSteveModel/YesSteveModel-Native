#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <c_string_view.h>
#include <java/array.h>
#include <java/opaque_ptr.h>
#include <java/ref.h>
#include <scope_guard.h>
#include <storage/bake_index.h>
#include <storage/cache_catalog.h>
#include <storage/player_data.h>

namespace ysm::storage {
class CacheRemote;
class Coordinator;
class FileIdAllocator;
}  // namespace ysm::storage

namespace ysm::lib::storage_bridge {

using Owner = std::variant<std::monostate, std::shared_ptr<storage::PlayerData>,
    std::shared_ptr<storage::Coordinator>, std::shared_ptr<storage::FileIdAllocator>,
    std::shared_ptr<storage::CacheCatalog>, std::shared_ptr<storage::CacheRemote>, std::shared_ptr<storage::BakeIndex>>;
using Payload = std::variant<std::monostate, std::vector<storage::PlayerFact>,
    std::vector<std::uint64_t>, std::vector<std::string>,
    std::vector<storage::CacheFile>, std::vector<storage::CacheSource>, std::vector<storage::ConvertedObject>,
    std::vector<storage::BakeObject>, std::vector<storage::CacheMaintenance>, std::vector<storage::CacheScope>>;

struct NativeResult {
    storage::Result header;
    Owner owner;
    Payload payload;
    void Set(storage::Result value) { header = value; }
    template <typename T>
    void Set(storage::Values<T> value) {
        header = value.result;
        payload = std::move(value.values);
    }
    template <typename T>
    void Set(std::pair<std::unique_ptr<T>, storage::Result> value) {
        header = value.second;
        if (value.first) owner = std::shared_ptr<T>(std::move(value.first));
    }
    std::size_t Count() const;
};

absl::Status CheckJni(JNIEnv_* env);
absl::StatusOr<std::string> TextInput(JNIEnv_* env, jstring value);
absl::StatusOr<std::string> BytesInput(JNIEnv_* env, jbyteArray value);
absl::StatusOr<std::filesystem::path> PathInput(JNIEnv_* env, jstring value);
absl::StatusOr<std::vector<std::string>> TextsInput(JNIEnv_* env, jobjectArray array, std::size_t size);

std::uint64_t Id(jlong value);
std::optional<std::uint64_t> After(jboolean present, jlong value);
std::array<jlong, 4> Header(storage::Result result, std::size_t count = 0);

// Allocate both the result backing and its opaque handle before any business
// effect. Until the primitive handle returns, this guard owns all cleanup.
template <typename Function>
absl::StatusOr<jlong> Complete(JNIEnv_* env, Function&& function) {
    const auto handle = java::MakeOpaquePtr<NativeResult>();
    auto cleanup = ScopeGuard([&] { java::DestroyOpaquePtr(handle); });
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    result->Set(function());
    YSM_RETURN_IF_ERROR(CheckJni(env));
    cleanup.Release();
    return handle;
}

template <typename Function>
absl::StatusOr<jlongArray> CompleteSimple(JNIEnv_* env, Function&& function) {
    // Allocate the only delivery carrier before a transition can commit.
    auto array = java::LocalRef(env, env->NewLongArray(4));
    YSM_RETURN_IF_ERROR(CheckJni(env));
    if (!array) return absl::InternalError("JVM returned null storage carrier"_sv);
    const auto values = Header(function());
    YSM_RETURN_IF_ERROR((java::WriteLongArray<true>(env, array.get(), values)));
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return array.release();
}

template <typename Function>
absl::StatusOr<jlongArray> CompleteReferences(JNIEnv_* env, jobjectArray containers, Function&& function) {
    const auto size = containers ? env->GetArrayLength(containers) : 0;
    if (size <= 0 || size > storage::kMaximumRows)
        return absl::InvalidArgumentError("Invalid reference batch"_sv);
    YSM_DECLARE_OR_RETURN(keys, TextsInput(env, containers, size));
    const auto result = function(keys);
    const auto count = result.values.size();
    if (count != (result.result.outcome == storage::Outcome::kFound ? keys.size() : 0))
        return absl::InternalError("Incomplete reference batch"_sv);

    const auto header = Header(result.result, count);
    std::array<jlong, 6 + storage::kMaximumRows> values;
    std::copy(header.begin(), header.end(), values.begin());
    if (count != 0) {
        // Coverage belongs to the batch snapshot; only the states vary by key.
        values[4] = result.values.front().domains;
        values[5] = result.values.front().incomplete_domains;
        for (std::size_t i = 0; i < count; ++i) values[6 + i] = static_cast<jlong>(result.values[i].state);
    }
    auto array = java::LongArray<true>(env, std::span(values).first(count == 0 ? 4 : 6 + count));
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return array.release();
}

}  // namespace ysm::lib::storage_bridge
