#include "java/string.h"

#include <string_cvt.h>
#include "java/class.h"

namespace ysm::java {
static_assert(sizeof(jchar) == 2);

jclass StrType(JNIEnv_* env) {
    static const jclass type = [&] {
        auto clazz = FindClass(env, "java/lang/String");
        if (!clazz.ok()) {
            throw std::runtime_error("jstring type not found");
        }
        return clazz.value();
    }();
    return type;
}

Ref<jstring> U8ToStr(JNIEnv_* env, CStringView str) {
    return LocalRef(env, env->NewStringUTF(str.c_str()));
}

std::string StrToU8(JNIEnv_* env, jstring str) {
    auto byte_len = env->GetStringUTFLength(str);
    std::string result_str(byte_len, '\0');
    // The region length counts UTF-16 code units, not output bytes.
    env->GetStringUTFRegion(str, 0, env->GetStringLength(str), result_str.data());
    return result_str;
}

#if YSM_WINDOWS
static_assert(sizeof(wchar_t) == 2);

fs::path StrToPath(JNIEnv_* env, jstring str) {
    auto len = env->GetStringLength(str);
    std::wstring result_str(len, L'\0');
    env->GetStringRegion(str, 0, len,
                         reinterpret_cast<jchar*>(result_str.data()));
    fs::path result(std::move(result_str));
    return result;
}

Ref<jstring> PathToStr(JNIEnv_* env, const fs::path& path) {
    auto& str = path.native();
    return LocalRef(
        env,
        env->NewString(reinterpret_cast<const jchar*>(str.data()), str.size()));
}
#else
fs::path StrToPath(JNIEnv_* env, jstring str) {
    auto result_str = StrToU8(env, str);
    fs::path result(std::move(result_str));
    return result;
}

Ref<jstring> PathToStr(JNIEnv_* env, const fs::path& path) {
    auto& str = path.native();
    return LocalRef(env, env->NewStringUTF(str.c_str()));
}
#endif
}  // namespace ysm::java
