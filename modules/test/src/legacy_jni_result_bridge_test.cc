#include <gtest/gtest.h>
#include <jni.h>

#include <cstdarg>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <java/opaque_ptr.h>
#include <legacy/result_bridge.h>
#include <legacy/result_owner.h>

namespace ysm::legacy::java {
namespace {
class FakeJni final {
   public:
    explicit FakeJni(int fail_call = -1) : fail_call_(fail_call) {
        env_.functions = &functions_;
        functions_.FindClass = FindClass;
        functions_.GetMethodID = GetMethodId;
        functions_.NewByteArray = NewByteArray;
        functions_.SetByteArrayRegion = SetByteArrayRegion;
        functions_.NewObjectArray = NewObjectArray;
        functions_.NewDirectByteBuffer = NewDirectByteBuffer;
        functions_.CallObjectMethodV = CallObjectMethodV;
        functions_.SetObjectArrayElement = SetObjectArrayElement;
        functions_.NewString = NewString;
        functions_.NewObjectV = NewObjectV;
        functions_.ExceptionCheck = ExceptionCheck;
        functions_.DeleteLocalRef = DeleteLocalRef;
    }

    JNIEnv_* env() noexcept { return &env_; }

    [[nodiscard]] int calls() const noexcept { return calls_; }

    [[nodiscard]] jlong transferred_owner() const noexcept {
        return transferred_owner_;
    }

   private:
    static FakeJni& Self(JNIEnv* env) noexcept {
        return *reinterpret_cast<FakeJni*>(env);
    }

    bool Fail() noexcept {
        ++calls_;
        if (calls_ == fail_call_) {
            pending_exception_ = true;
            return true;
        }
        return false;
    }

    template <typename T>
    T Object() noexcept {
        ++next_object_;
        return reinterpret_cast<T>(next_object_);
    }

    static jclass JNICALL FindClass(JNIEnv* env, const char*) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jclass>();
    }

    static jmethodID JNICALL GetMethodId(JNIEnv* env, jclass, const char*,
                                         const char*) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jmethodID>();
    }

    static jbyteArray JNICALL NewByteArray(JNIEnv* env, jsize) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jbyteArray>();
    }

    static void JNICALL SetByteArrayRegion(JNIEnv* env, jbyteArray, jsize,
                                           jsize, const jbyte*) {
        Self(env).Fail();
    }

    static jobjectArray JNICALL NewObjectArray(JNIEnv* env, jsize, jclass,
                                               jobject) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jobjectArray>();
    }

    static jobject JNICALL NewDirectByteBuffer(JNIEnv* env, void*, jlong) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jobject>();
    }

    static jobject JNICALL CallObjectMethodV(JNIEnv* env, jobject, jmethodID,
                                             va_list) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jobject>();
    }

    static void JNICALL SetObjectArrayElement(JNIEnv* env, jobjectArray, jsize,
                                              jobject) {
        Self(env).Fail();
    }

    static jstring JNICALL NewString(JNIEnv* env, const jchar*, jsize) {
        auto& self = Self(env);
        return self.Fail() ? nullptr : self.Object<jstring>();
    }

    static jobject JNICALL NewObjectV(JNIEnv* env, jclass, jmethodID,
                                      va_list args) {
        auto& self = Self(env);
        if (self.Fail()) {
            return nullptr;
        }
        static_cast<void>(va_arg(args, jint));
        static_cast<void>(va_arg(args, jstring));
        const auto handle = va_arg(args, jlong);
        self.transferred_owner_ = handle;
        return self.Object<jobject>();
    }

    static jboolean JNICALL ExceptionCheck(JNIEnv* env) {
        return Self(env).pending_exception_ ? JNI_TRUE : JNI_FALSE;
    }

    static void JNICALL DeleteLocalRef(JNIEnv*, jobject) {}

    JNIEnv_ env_{};
    JNINativeInterface_ functions_{};
    int fail_call_;
    int calls_{};
    std::uintptr_t next_object_{0x1000};
    bool pending_exception_{};
    jlong transferred_owner_{};
};

Payload MakePayload(PayloadKind kind, std::uint32_t logical_id,
                    std::string name, std::string_view bytes = {}) {
    return Payload{kind,         PayloadEncoding::kDirect,
                   logical_id,   std::move(name),
                   std::nullopt, BufferManaged(StrBuf(bytes))};
}

ImportResult MakeResult() {
    std::vector<Payload> payloads;
    payloads.emplace_back(
        MakePayload(PayloadKind::kManifest, 0, "", "manifest"));
    payloads.emplace_back(MakePayload(PayloadKind::kStringData, 1, ""));
    payloads.emplace_back(
        MakePayload(PayloadKind::kModelData, 2, "player", "model"));
    return ImportResult{{1, 64, {}}, std::move(payloads)};
}

TEST(LegacyJniResultBridgeTest, TransfersOnlyAfterTheCompleteJavaResult) {
    FakeJni env;
    auto result = NewSuccessResult(env.env(), MakeResult());
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_NE(result.value(), nullptr);
    ASSERT_NE(env.transferred_owner(), 0);
    auto retained =
        ::ysm::java::AcquireOpaquePtr<ResultOwner>(env.transferred_owner());
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(retained->use_count(), 2);
    ::ysm::java::DestroyOpaquePtr(env.transferred_owner());
    EXPECT_EQ(retained->use_count(), 1);
}

TEST(LegacyJniResultBridgeTest,
     EveryConstructionFailureRetainsNativeOwnership) {
    FakeJni successful;
    auto successful_result = NewSuccessResult(successful.env(), MakeResult());
    ASSERT_TRUE(successful_result.ok()) << successful_result.status();
    const int construction_calls = successful.calls();
    ::ysm::java::DestroyOpaquePtr(successful.transferred_owner());

    for (int fail_call = 1; fail_call <= construction_calls; ++fail_call) {
        FakeJni failing(fail_call);
        auto result = NewSuccessResult(failing.env(), MakeResult());
        EXPECT_FALSE(result.ok()) << "fail call " << fail_call;
        EXPECT_EQ(failing.transferred_owner(), 0) << "fail call " << fail_call;
    }
}
}  // namespace
}  // namespace ysm::legacy::java
