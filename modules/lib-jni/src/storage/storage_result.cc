#include "bridge.h"

#include <bit>
#include <concepts>
#include <functional>
#include <string_view>
#include <type_traits>

#include <java/entry.h>
#include <java/string.h>
#include <storage/cache_remote.h>
#include <storage/coordinator.h>
#include <storage/file_id_allocator.h>

namespace ysm::lib::storage_bridge {

std::size_t NativeResult::Count() const {
    return std::visit([](const auto& values) -> std::size_t {
        if constexpr (std::same_as<std::decay_t<decltype(values)>, std::monostate>) return 0;
        else return values.size();
    }, payload);
}

namespace {

template <typename T>
absl::StatusOr<std::reference_wrapper<const T>> Value(NativeResult& result, jint row) {
    const auto* values = std::get_if<std::vector<T>>(&result.payload);
    if (!values || row < 0 || static_cast<std::size_t>(row) >= values->size())
        return absl::InvalidArgumentError("Storage result payload or index mismatch"_sv);
    return std::cref((*values)[static_cast<std::size_t>(row)]);
}
absl::StatusOr<jstring> TextOutput(JNIEnv_* env, CStringView value) {
    auto text = java::U8ToStr(env, value);
    YSM_RETURN_IF_ERROR(CheckJni(env));
    if (!text) return absl::InternalError("JVM returned null text"_sv);
    return text.release();
}
absl::StatusOr<jbyteArray> BytesOutput(JNIEnv_* env, std::string_view value) {
    auto bytes = java::ByteArray<true>(env, value);
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return bytes.release();
}

jlong LongBits(std::uint64_t value) { return std::bit_cast<jlong>(value); }

// Primitive carriers are private JNI representation. Public callers consume
// named records; native never constructs or reflects on Java business objects.
absl::StatusOr<std::string> CacheText(NativeResult& result, jint row, jint field) {
    return std::visit([&](const auto& values) -> absl::StatusOr<std::string> {
        using V = std::decay_t<decltype(values)>;
        if constexpr (std::same_as<V, std::monostate>) return absl::InvalidArgumentError("Absent cache payload"_sv);
        else {
            if (row < 0 || static_cast<std::size_t>(row) >= values.size()) return absl::InvalidArgumentError("Invalid cache row"_sv);
            const auto& value = values[row];
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<T, storage::CacheScope>) {
                if (field == 0) return value.scope;
            } else if constexpr (std::same_as<T, storage::CacheFile>) {
                if (field == 0) return value.location.instance;
            } else if constexpr (std::same_as<T, storage::ConvertedObject>) {
                switch (field) { case 0: return value.location.instance; case 1: return value.scope;
                    case 2: return value.cache_version; case 3: return value.model; case 4: return value.container; }
            } else if constexpr (std::same_as<T, storage::CacheSource>) {
                switch (field) { case 0: return value.source; case 1: return value.model; case 2: return value.container;
                    case 3: return value.converted ? value.converted->location.instance : "";
                    case 4: return value.converted ? value.converted->scope : "";
                    case 5: return value.converted ? value.converted->cache_version : ""; }
            } else if constexpr (std::same_as<T, storage::BakeObject>) {
                const auto& key = value.key; const auto& partition = key.partition;
                switch (field) { case 0: return value.file.location.instance; case 1: return partition.cache_version;
                    case 2: return partition.container; case 3: return partition.binding; case 4: return partition.purpose;
                    case 5: return key.exact; case 6: return key.abi; case 7: return key.capability;
                    case 8: return key.engine_package; case 9: return key.compilation; case 10: return key.frozen; }
            }
            return absl::InvalidArgumentError("Invalid cache text field"_sv);
        }
    }, result.payload);
}
absl::StatusOr<jlong> CacheNumber(NativeResult& result, jint row, jint field) {
    return std::visit([&](const auto& values) -> absl::StatusOr<jlong> {
        using V = std::decay_t<decltype(values)>;
        if constexpr (std::same_as<V, std::monostate>) return absl::InvalidArgumentError("Absent cache payload"_sv);
        else {
            if (row < 0 || static_cast<std::size_t>(row) >= values.size()) return absl::InvalidArgumentError("Invalid cache row"_sv);
            const auto& value = values[row]; using T = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<T, std::uint64_t>) { if (field == 0) return LongBits(value); }
            else if constexpr (std::same_as<T, storage::CacheScope>) { if (field == 0) return value.complete; }
            else if constexpr (std::same_as<T, storage::CacheFile>) { if (field == 0) return LongBits(value.location.file_id); }
            else if constexpr (std::same_as<T, storage::ConvertedObject>) {
                if (field == 0) return LongBits(value.location.file_id); if (field == 1) return value.reusable;
            } else if constexpr (std::same_as<T, storage::CacheSource>) {
                if (field == 0) return value.converted.has_value();
                if (field == 1) return LongBits(value.converted ? value.converted->location.file_id : 0);
            } else if constexpr (std::same_as<T, storage::BakeObject>) {
                if (field == 0) return LongBits(value.file.location.file_id); if (field == 1) return static_cast<jlong>(value.key.partition.kind);
            } else if constexpr (std::same_as<T, storage::CacheMaintenance>) {
                if (field == 0) return value.last_success_utc.has_value(); if (field == 1) return value.last_success_utc.value_or(0);
                if (field == 2) return LongBits(value.reset_revision); if (field == 3) return value.pending_all;
            }
            return absl::InvalidArgumentError("Invalid cache number field"_sv);
        }
    }, result.payload);
}
absl::StatusOr<std::string> CacheBytes(NativeResult& result, jint row, jint field) {
    return std::visit([&](const auto& values) -> absl::StatusOr<std::string> {
        using V = std::decay_t<decltype(values)>;
        if constexpr (std::same_as<V, std::monostate>) return absl::InvalidArgumentError("Absent cache payload"_sv);
        else {
            if (row < 0 || static_cast<std::size_t>(row) >= values.size()) return absl::InvalidArgumentError("Invalid cache row"_sv);
            const auto& value = values[row]; using T = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<T, std::string>) { if (field == 0) return value; }
            else if constexpr (std::same_as<T, storage::CacheFile>) { if (field == 0) return value.metadata; }
            else if constexpr (std::same_as<T, storage::CacheSource>) {
                if (field == 0) return value.observation; if (field == 1) return value.metadata;
            } else if constexpr (std::same_as<T, storage::BakeObject>) { if (field == 0) return value.file.metadata; }
            return absl::InvalidArgumentError("Invalid cache byte field"_sv);
        }
    }, result.payload);
}

}  // namespace

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nHeader(J)[J", (handle)) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    const auto values = Header(result->header, result->Count());
    auto array = java::LongArray<true>(env, values);
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return array.release();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nTakeOwner(J)J", (handle), jlong{0}) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    const auto owner = std::visit([](const auto& value) -> jlong {
        if constexpr (std::same_as<std::decay_t<decltype(value)>, std::monostate>) return 0;
        else return java::ShareOpaquePtr(value);
    }, result->owner);
    if (!owner) return absl::FailedPreconditionError("Storage result has no unclaimed business owner"_sv);
    result->owner = std::monostate{};
    return owner;
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nPreferenceIdentity(JI)Ljava/lang/String;", (handle, row)) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    YSM_DECLARE_OR_RETURN(value, Value<storage::PlayerFact>(*result, row));
    return TextOutput(env, value.identity);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nPreferenceValue(JI)[B", (handle, row)) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    YSM_DECLARE_OR_RETURN(value, Value<storage::PlayerFact>(*result, row));
    return BytesOutput(env, value.value);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nPreferenceTime(JI)J", (handle, row), jlong{-1}) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    YSM_DECLARE_OR_RETURN(value, Value<storage::PlayerFact>(*result, row));
    return static_cast<jlong>(value.updated_utc_seconds);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nCacheText(JII)Ljava/lang/String;", (handle, row, field)) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    YSM_DECLARE_OR_RETURN(value, CacheText(*result, row, field)); return TextOutput(env, value);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nCacheNumber(JII)[J", (handle, row, field)) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    YSM_DECLARE_OR_RETURN(value, CacheNumber(*result, row, field));
    // Array null remains distinguishable from every valid uint64 bit pattern.
    auto array = java::LongArray<true>(env, std::array<jlong, 1>{value}); YSM_RETURN_IF_ERROR(CheckJni(env)); return array.release();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeStorageResult;nCacheBytes(JII)[B", (handle, row, field)) {
    YSM_DECLARE_OR_RETURN(result, java::AcquireOpaquePtr<NativeResult>(handle));
    YSM_DECLARE_OR_RETURN(value, CacheBytes(*result, row, field)); return BytesOutput(env, value);
}

}  // namespace ysm::lib::storage_bridge
