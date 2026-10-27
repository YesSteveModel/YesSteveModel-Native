#include <java/entry.h>
#include <java/array.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

#include <bitpack.h>
#include <log.h>

#include "cpu.h"
#include "device_quality.h"
#include "profile.h"
#include "thread_scheduling.h"

#if YSM_MACOS
#include "mac_user_directories.h"
#endif

namespace ysm::lib::runtime {
namespace {
YSM_BIT_PACK(PackedJavaConfig, jlong,
             YSM_BIT_FIELD(log_level, uint8_t, 3));
YSM_BIT_PACK(PackedNativeConfig, jlong,
             YSM_BIT_FIELD(tracy_enabled, uint8_t, 1),
             YSM_BIT_FIELD(debug_logging_enabled, uint8_t, 1));

std::mutex g_initialize_mutex;
std::optional<jlong> g_initialized_java_config;

absl::StatusOr<jlong> NativeConfig() {
    return PackedNativeConfig::pack(YSM_ENABLE_TRACY != 0,
                                    YSM_ENABLE_DEBUG_LOG != 0);
}
}  // namespace

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeRuntime;nPhysicalCoreCount()I",
              (), jint{0}) {
    YSM_DECLARE_OR_RETURN(cores, QueryPhysicalCoreCount());
    YSM_ASSERT(cores > 0 && cores <= (std::numeric_limits<jint>::max)(),
               absl::OutOfRangeError("Physical core count is outside the JNI range"));
    return static_cast<jint>(cores);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeRuntime;nMacUserDirectories()[Ljava/lang/String;",
              (), jobjectArray{nullptr}) {
#if YSM_MACOS
    return QueryMacUserDirectories(env);
#else
    return absl::UnimplementedError("User-domain Foundation directories require macOS"_sv);
#endif
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeRuntime;nApplyThreadPolicy(I)[J",
              (policy), jlongArray{nullptr}) {
    const auto report = ApplyCurrentThreadPolicy(static_cast<ThreadPolicy>(policy));
    const auto values = report.Encode();
    return java::LongArray<true>(env, std::span(values).first(report.EncodedSize())).release();
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeRuntime;nClassifyDevice(J)I",
              (max_heap_bytes), jint{0}) {
    YSM_LOG(INFO, "Device classification requested: maximum heap {} bytes", max_heap_bytes);
    const auto quality = QueryDeviceQuality(max_heap_bytes);
    if (!quality.ok()) {
        YSM_LOG(WARNING, "Device classification failed: {}", quality.status().message());
        return quality.status();
    }
    return static_cast<jint>(*quality);
}

YSM_JNI_ENTRY("Lcom/elfmcys/ysm/natives/NativeRuntime;nInitialize(J)J",
              (java_config), jlong{-1}) {
    const auto [log_level] =
        PackedJavaConfig::unpack(java_config);
    YSM_DECLARE_OR_RETURN(canonical_java_config,
                          PackedJavaConfig::pack(log_level));
    YSM_ASSERT(canonical_java_config == java_config,
               absl::InvalidArgumentError("Unknown Java runtime config bits"));
    YSM_DECLARE_OR_RETURN(native_config, NativeConfig());

    std::scoped_lock lock(g_initialize_mutex);
    if (g_initialized_java_config.has_value()) {
        YSM_ASSERT(*g_initialized_java_config == canonical_java_config,
                   absl::FailedPreconditionError(
                       "Native runtime was initialized with different config"));
        return native_config;
    }

    YSM_RETURN_IF_ERROR(ConfigureLogLevel(static_cast<int>(log_level)));
    g_initialized_java_config = canonical_java_config;
    YSM_LOG(INFO,
            "Native runtime initialized: log level {}, tracy {}, debug logging {}",
            LogLevelName(static_cast<LogLevel>(log_level)),
            YSM_ENABLE_TRACY != 0, YSM_ENABLE_DEBUG_LOG != 0);
    return native_config;
}
}  // namespace ysm::lib::runtime
