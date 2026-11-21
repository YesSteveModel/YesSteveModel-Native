#include "bridge.h"

#include <algorithm>
#include <span>

#include <java/entry.h>
#include <storage/cache_remote.h>

namespace ysm::lib::storage_bridge {

namespace {

template <typename Function>
absl::StatusOr<jlongArray> CompleteUsage(JNIEnv_* env, Function&& function) {
    const auto result = function();
    if (result.values.size() > 1) return absl::InternalError("Invalid usage result count"_sv);
    const auto header = Header(result.result, result.values.size());
    std::array<jlong, 5> values;
    std::copy(header.begin(), header.end(), values.begin());
    if (!result.values.empty()) values[4] = result.values.front();
    auto array = java::LongArray<true>(env, std::span(values).first(4 + result.values.size()));
    YSM_RETURN_IF_ERROR(CheckJni(env));
    return array.release();
}

}  // namespace

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nOpen(Ljava/lang/String;ZJ)J", (root, existing_only, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(path, PathInput(env, root));
    return Complete(env, [&] { return storage::CacheRemote::Open(path, *token, existing_only); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nLookupExact(JILjava/lang/String;J)J", (handle, kind, key, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, key));
    return Complete(env, [&] { return owner->LookupExact(static_cast<storage::ArtifactKind>(kind), text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nPublishExact(JILjava/lang/String;JLjava/lang/String;[BJ)[J", (handle, kind, key, id, instance, metadata, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, key));
    YSM_DECLARE_OR_RETURN(token_text, TextInput(env, instance));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, metadata));
    return CompleteSimple(env, [&] { return owner->PublishExact(static_cast<storage::ArtifactKind>(kind), text, {{Id(id), token_text}, bytes}, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nReferencesMany(J[Ljava/lang/String;J)[J", (handle, containers, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteReferences(env, containers, [&](auto& keys) { return owner->ReferencesMany(keys, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nPublishMetadata(JLjava/lang/String;JLjava/lang/String;[BLjava/lang/String;Ljava/lang/String;J)[J", (handle, source, id, instance, metadata, model, container, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(source_text, TextInput(env, source));
    YSM_DECLARE_OR_RETURN(instance_text, TextInput(env, instance));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, metadata));
    YSM_DECLARE_OR_RETURN(model_text, TextInput(env, model));
    YSM_DECLARE_OR_RETURN(container_text, TextInput(env, container));
    return CompleteSimple(env, [&] { return owner->PublishMetadata(source_text, {{Id(id), instance_text}, bytes}, model_text, container_text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nSetReference(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;ZJ)[J", (handle, source, model, container, present, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(source_text, TextInput(env, source));
    YSM_DECLARE_OR_RETURN(model_text, TextInput(env, model));
    YSM_DECLARE_OR_RETURN(container_text, TextInput(env, container));
    return CompleteSimple(env, [&] { return owner->SetReference(source_text, model_text, container_text, present, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nSetReferencesComplete(JZJ)[J", (handle, complete, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteSimple(env, [&] { return owner->SetReferencesComplete(complete, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nMaxUsage(JLjava/lang/String;JJ)[J", (handle, container, utc, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, container));
    return CompleteSimple(env, [&] { return owner->MaxUsage(text, utc, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheRemote;nReadUsage(JLjava/lang/String;J)[J", (handle, container, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheRemote>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, container));
    return CompleteUsage(env, [&] { return owner->ReadUsage(text, *token); });
}

}  // namespace ysm::lib::storage_bridge
