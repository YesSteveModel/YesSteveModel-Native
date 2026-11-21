#include "bridge.h"

#include <java/entry.h>

namespace ysm::lib::storage_bridge {

namespace {

storage::BakePartition Partition(jint kind, const std::vector<std::string>& text) {
    return {static_cast<storage::BakeKind>(kind), text[0], text[1], text[2], text[3]};
}
storage::BakeKey Key(jint kind, const std::vector<std::string>& text) {
    return {Partition(kind, text), text[4], text[5], text[6], text[7], text[8], text[9]};
}

}  // namespace

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nOpen(Ljava/lang/String;ZJ)J", (root, read_only, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(path, PathInput(env, root));
    return Complete(env, [&] { return storage::BakeIndex::Open(path, read_only, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nPageObjects(JZJJ)J", (handle, has_after, after, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return Complete(env, [&] { return owner->PageObjects(After(has_after, after), *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nRevoke(JJLjava/lang/String;J)[J", (handle, id, instance, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, instance));
    return CompleteSimple(env, [&] { return owner->Revoke({Id(id), text}, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nForget(JJLjava/lang/String;J)[J", (handle, id, instance, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextInput(env, instance));
    return CompleteSimple(env, [&] { return owner->Forget({Id(id), text}, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nLookupExact(JI[Ljava/lang/String;J)J", (handle, kind, fields, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextsInput(env, fields, 10));
    return Complete(env, [&] { return owner->LookupExact(Key(kind, text), *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nHasPartition(JI[Ljava/lang/String;J)J", (handle, kind, fields, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextsInput(env, fields, 4));
    return Complete(env, [&] {
        auto result = owner->HasPartition(Partition(kind, text), *token);
        storage::Values<std::uint64_t> output{.result = result.result};
        for (const auto value : result.values) output.values.push_back(value);
        return output;
    });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nOwnsFile(JJJ)J", (handle, id, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    return Complete(env, [&] {
        auto result = owner->OwnsFile(Id(id), *token);
        storage::Values<std::uint64_t> output{.result = result.result};
        for (const auto value : result.values) output.values.push_back(value);
        return output;
    });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativeBakeIndex;nPublish(JI[Ljava/lang/String;JLjava/lang/String;[BJ)[J", (handle, kind, fields, id, instance, metadata, cancelled)) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::BakeIndex>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(text, TextsInput(env, fields, 10));
    YSM_DECLARE_OR_RETURN(token_text, TextInput(env, instance));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, metadata));
    return CompleteSimple(env, [&] { return owner->Publish({{{Id(id), token_text}, bytes}, Key(kind, text)}, *token); });
}

}  // namespace ysm::lib::storage_bridge
