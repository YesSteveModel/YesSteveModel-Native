#include "bridge.h"

#include <java/entry.h>

namespace ysm::lib::storage_bridge {

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nOpen(Ljava/lang/String;ZJ)J", (root, existing_only, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(path, PathInput(env, root));
    return Complete(env, [&] { return storage::CacheCatalog::Open(path, *token, existing_only); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nLookupExact(JILjava/lang/String;J)J", (handle, kind, key, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, key));
    return Complete(env, [&] { return owner->LookupExact(static_cast<storage::ArtifactKind>(kind), text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPublishExact(JILjava/lang/String;JLjava/lang/String;[BJ)[J", (handle, kind, key, id, instance, metadata, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, key));
    YSM_DECLARE_OR_RETURN(token_text, TextInput(env, instance));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, metadata));
    return CompleteSimple(env, [&] { return owner->PublishExact(static_cast<storage::ArtifactKind>(kind), text, {{Id(id), token_text}, bytes}, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nLookupDirectory(JLjava/lang/String;J)J", (handle, key, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, key));
    return Complete(env, [&] { return owner->LookupDirectory(text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPublishDirectory(JLjava/lang/String;[BJ)[J", (handle, key, metadata, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, key));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, metadata));
    return CompleteSimple(env, [&] { return owner->PublishDirectory(text, bytes, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nLookupSource(JLjava/lang/String;Ljava/lang/String;J)J", (handle, scope, source, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(scope_text, TextInput(env, scope));
    YSM_DECLARE_OR_RETURN(source_text, TextInput(env, source));
    return Complete(env, [&] { return owner->LookupSource(scope_text, source_text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPageSources(JLjava/lang/String;Ljava/lang/String;J)J", (handle, scope, source, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(scope_text, TextInput(env, scope));
    YSM_DECLARE_OR_RETURN(source_text, TextInput(env, source));
    return Complete(env, [&] { return owner->PageSources(scope_text, source_text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPublishSource(J[Ljava/lang/String;ZJ[B[BJ)[J", (handle, fields, has_object, id, observation, metadata, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextsInput(env, fields, 6));
    YSM_DECLARE_OR_RETURN(observed, BytesInput(env, observation));
    YSM_DECLARE_OR_RETURN(details, BytesInput(env, metadata));
    storage::CacheSource source{text[1], text[2], text[3], std::nullopt, observed, details};
    if (has_object) source.converted = storage::ConvertedObject{{Id(id), text[4]}, text[0], text[5], text[2], text[3], true};
    return CompleteSimple(env, [&] { return owner->PublishSource(text[0], source, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPublishSourceReferences(JLjava/lang/String;[Ljava/lang/String;J)[J", (handle, scope, fields, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(scope_text, TextInput(env, scope));
    const auto size = fields ? env->GetArrayLength(fields) : 0;
    if (size == 0 || size % 3 != 0 || size / 3 > storage::kMaximumRows)
        return absl::InvalidArgumentError("Invalid source reference batch"_sv);
    YSM_DECLARE_OR_RETURN(text, TextsInput(env, fields, size));
    std::vector<storage::SourceReference> sources;
    sources.reserve(size / 3);
    for (jsize i = 0; i < size; i += 3) sources.push_back({text[i], text[i + 1], text[i + 2]});
    return CompleteSimple(env, [&] { return owner->PublishSourceReferences(scope_text, sources, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nSetScopeComplete(JLjava/lang/String;ZJ)[J", (handle, scope, complete, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, scope));
    return CompleteSimple(env, [&] { return owner->SetScopeComplete(text, complete, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nFinishScan(JLjava/lang/String;[Ljava/lang/String;J)[J", (handle, scope, sources, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, scope));
    if (!sources || env->GetArrayLength(sources) >= storage::kMaximumRows) return absl::InvalidArgumentError("Invalid source retirement"_sv);
    YSM_DECLARE_OR_RETURN(keys, TextsInput(env, sources, env->GetArrayLength(sources)));
    return CompleteSimple(env, [&] { return owner->FinishScan(text, keys, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nReferencesMany(J[Ljava/lang/String;J)[J", (handle, containers, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteReferences(env, containers, [&](auto& keys) { return owner->ReferencesMany(keys, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPageConverted(JZJJ)J", (handle, has_after, after, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return Complete(env, [&] { return owner->PageConverted(After(has_after, after), *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nRevokeConverted(JJLjava/lang/String;J)[J", (handle, id, instance, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, instance));
    return CompleteSimple(env, [&] { return owner->RevokeConverted({Id(id), text}, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nForgetConverted(JJLjava/lang/String;J)[J", (handle, id, instance, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, instance));
    return CompleteSimple(env, [&] { return owner->ForgetConverted({Id(id), text}, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nReadMaintenance(JJ)J", (handle, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return Complete(env, [&] { return owner->ReadMaintenance(*token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nResetMaintenance(JZJ)[J", (handle, all, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteSimple(env, [&] { return owner->ResetMaintenance(all, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nCompleteMaintenance(JJJJ)[J", (handle, revision, utc, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteSimple(env, [&] { return owner->CompleteMaintenance(Id(revision), utc, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nOwnsFile(JJJ)J", (handle, id, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return Complete(env, [&] {
        auto result = owner->OwnsFile(Id(id), *token);
        storage::Values<std::uint64_t> output{.result = result.result};
        for (const auto value : result.values) output.values.push_back(value);
        return output;
    });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPageScopes(JLjava/lang/String;J)J", (handle, after, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, after));
    return Complete(env, [&] { return owner->PageScopes(text, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nPageConvertedScope(JLjava/lang/String;ZJJ)J", (handle, scope, has_after, after, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, scope));
    return Complete(env, [&] { return owner->PageConvertedScope(text, After(has_after, after), *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeCacheCatalog;nSetReferencesComplete(JZJ)[J", (handle, complete, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::CacheCatalog>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return CompleteSimple(env, [&] { return owner->SetReferencesComplete(complete, *token); });
}

}  // namespace ysm::lib::storage_bridge
