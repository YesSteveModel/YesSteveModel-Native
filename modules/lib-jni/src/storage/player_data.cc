#include "bridge.h"

#include <java/entry.h>

namespace ysm::lib::storage_bridge {

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativePlayerData;nOpen(Ljava/lang/String;J)J", (root, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(path, PathInput(env, root));
    return Complete(env, [&] { return storage::PlayerData::Open(path, *token); });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativePlayerData;nReadPreference(JLjava/lang/String;ILjava/lang/String;J)J",
    (handle, player, kind, identity, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::PlayerData>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(player_text, TextInput(env, player));
    YSM_DECLARE_OR_RETURN(identity_text, TextInput(env, identity));
    return Complete(env, [&] {
        return owner->ReadPreference(player_text, static_cast<storage::FactKind>(kind), identity_text, *token);
    });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativePlayerData;nPutPreference(JLjava/lang/String;ILjava/lang/String;[BJJ)J",
    (handle, player, kind, identity, value, utc_seconds, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::PlayerData>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(player_text, TextInput(env, player));
    YSM_DECLARE_OR_RETURN(identity_text, TextInput(env, identity));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, value));
    return Complete(env, [&] {
        return owner->PutPreference(player_text, static_cast<storage::FactKind>(kind), identity_text, bytes, utc_seconds, *token);
    });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativePlayerData;nInsertIfAbsentAndRead(JLjava/lang/String;ILjava/lang/String;[BJJ)J",
    (handle, player, kind, identity, value, utc_seconds, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::PlayerData>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(player_text, TextInput(env, player));
    YSM_DECLARE_OR_RETURN(identity_text, TextInput(env, identity));
    YSM_DECLARE_OR_RETURN(bytes, BytesInput(env, value));
    return Complete(env, [&] {
        return owner->InsertIfAbsentAndRead(player_text, static_cast<storage::FactKind>(kind), identity_text, bytes, utc_seconds, *token);
    });
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/storage/NativePlayerData;nPagePreferences(JLjava/lang/String;ILjava/lang/String;J)J",
    (handle, player, kind, after_identity, cancelled), jlong{0}) {
    YSM_DECLARE_OR_RETURN(owner, java::AcquireOpaquePtr<storage::PlayerData>(handle));
    YSM_DECLARE_OR_RETURN(token, java::AcquireOpaquePtr<CancellationState>(cancelled));
    YSM_DECLARE_OR_RETURN(player_text, TextInput(env, player));
    YSM_DECLARE_OR_RETURN(after_text, TextInput(env, after_identity));
    return Complete(env, [&] {
        return owner->PagePreferences(player_text, static_cast<storage::FactKind>(kind), after_text, *token);
    });
}

}  // namespace ysm::lib::storage_bridge
