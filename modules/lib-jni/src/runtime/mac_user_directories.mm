#include "mac_user_directories.h"

#import <Foundation/Foundation.h>

#include <array>
#include <java/string.h>
#include <limits>
#include <vector>

#include "err.h"

namespace ysm::lib::runtime {

absl::StatusOr<jobjectArray> QueryMacUserDirectories(JNIEnv_* env) {
    @try {
        @autoreleasepool {
            const auto string_type = java::StrType(env);
            YSM_ASSERT(string_type != nullptr,
                       absl::InternalError("Java String class is unavailable"_sv));
            auto result = java::LocalRef(env, env->NewObjectArray(2, string_type, nullptr));
            YSM_ASSERT(result.get() != nullptr,
                       absl::ResourceExhaustedError("Cannot allocate macOS directory report"_sv));

            constexpr std::array directories{NSApplicationSupportDirectory, NSCachesDirectory};
            for (jsize index = 0; index < static_cast<jsize>(directories.size()); ++index) {
                NSString* path = NSSearchPathForDirectoriesInDomains(
                    directories[index], NSUserDomainMask, YES).firstObject;
                if (path == nil) continue;
                const auto length = path.length;
                YSM_ASSERT(length > 0 && length <= static_cast<NSUInteger>(
                    (std::numeric_limits<jsize>::max)()),
                    absl::OutOfRangeError("Foundation directory length is outside the JNI range"_sv));
                std::vector<jchar> utf16(length);
                [path getCharacters:utf16.data() range:NSMakeRange(0, length)];
                // Foundation and JNI share UTF-16; NewStringUTF would misread supplementary characters.
                auto value = java::LocalRef(env, env->NewString(utf16.data(), static_cast<jsize>(length)));
                YSM_ASSERT(value.get() != nullptr,
                           absl::ResourceExhaustedError("Cannot copy a Foundation directory"_sv));
                env->SetObjectArrayElement(result.get(), index, value.get());
                YSM_ASSERT(!env->ExceptionCheck(),
                           absl::InternalError("Cannot fill the macOS directory report"_sv));
            }
            return result.release();
        }
    } @catch (NSException*) {
        return absl::InternalError("Foundation directory query raised an exception"_sv);
    }
}

}  // namespace ysm::lib::runtime
