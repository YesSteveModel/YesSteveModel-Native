#include "bridge.h"

#include <bit>

#include <java/string.h>

namespace ysm::lib::storage_bridge {

absl::Status CheckJni(JNIEnv_* env) {
    return env->ExceptionCheck() ? absl::InternalError("JVM rejected storage carrier"_sv) : absl::OkStatus();
}
absl::StatusOr<std::string> TextInput(JNIEnv_* env, jstring value) {
    if (!value || env->GetStringLength(value) > 4096) return absl::InvalidArgumentError("Invalid storage text"_sv);
    auto text = java::StrToU8(env, value);
    YSM_RETURN_IF_ERROR(CheckJni(env));
    if (text.size() > 4096 || text.find('\0') != std::string::npos)
        return absl::InvalidArgumentError("Invalid storage text representation"_sv);
    return text;
}
absl::StatusOr<std::string> BytesInput(JNIEnv_* env, jbyteArray value) {
    if (!value) return absl::InvalidArgumentError("Null storage bytes"_sv);
    const auto size = env->GetArrayLength(value);
    if (size > storage::kMaximumBytes) return absl::ResourceExhaustedError("Storage payload exceeds bound"_sv);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    YSM_RETURN_IF_ERROR((java::ReadByteArray<true>(env, value, bytes)));
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return bytes;
}
absl::StatusOr<std::filesystem::path> PathInput(JNIEnv_* env, jstring value) {
    if (!value) return absl::InvalidArgumentError("Null business location"_sv);
    auto path = java::StrToPath(env, value);
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return path;
}

absl::StatusOr<std::vector<std::string>> TextsInput(JNIEnv_* env, jobjectArray array, std::size_t size) {
    if (!array || env->GetArrayLength(array) != size) return absl::InvalidArgumentError("Invalid cache record carrier"_sv);
    std::vector<std::string> output; output.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        auto local = java::LocalRef(env, static_cast<jstring>(env->GetObjectArrayElement(array, static_cast<jsize>(index))));
        YSM_RETURN_IF_ERROR(CheckJni(env));
        YSM_DECLARE_OR_RETURN(text, TextInput(env, local.get())); output.push_back(std::move(text));
    }
    return output;
}
std::uint64_t Id(jlong value) { return std::bit_cast<std::uint64_t>(value); }
std::optional<std::uint64_t> After(jboolean present, jlong value) {
    return present ? std::optional(Id(value)) : std::nullopt;
}

std::array<jlong, 4> Header(storage::Result result, std::size_t count) {
    return {static_cast<jlong>(result.outcome), static_cast<jlong>(result.failure),
        static_cast<jlong>(result.committed), static_cast<jlong>(count)};
}

}  // namespace ysm::lib::storage_bridge
