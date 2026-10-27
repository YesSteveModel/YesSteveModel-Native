#include <gtest/gtest.h>
#include <java/buffer.h>

#include "bake/model_result.h"

namespace ysm::test {
namespace {
struct FakeJni {
    JNIEnv_ env{};
    JNINativeInterface_ functions{};
    bool pending = false;
    bool fail_mapping = false;
    bool null_output = false;
    bool fail_output = false;
    int mapping_writes = 0;
    int deleted_refs = 0;
    void* output_memory = nullptr;
    std::array<jshort, 2> mapping{};

    FakeJni() {
        env.functions = &functions;
        functions.ExceptionCheck = [](JNIEnv_* env) -> jboolean {
            return Self(env).pending;
        };
        functions.GetArrayLength = [](JNIEnv_*, jarray) -> jsize { return 2; };
        functions.SetShortArrayRegion = [](JNIEnv_* env, jshortArray, jsize,
                                          jsize count, const jshort* data) {
            auto& self = Self(env);
            ++self.mapping_writes;
            std::copy_n(data, count, self.mapping.begin());
            self.pending = self.fail_mapping;
        };
        functions.NewDirectByteBuffer = [](JNIEnv_* env, void* ptr, jlong) -> jobject {
            auto& self = Self(env);
            self.output_memory = ptr;
            self.pending = self.fail_output;
            return self.null_output ? nullptr : reinterpret_cast<jobject>(1);
        };
        functions.DeleteLocalRef = [](JNIEnv_* env, jobject) {
            ++Self(env).deleted_refs;
        };
    }

    static FakeJni& Self(JNIEnv_* env) {
        return *reinterpret_cast<FakeJni*>(env);
    }
};

std::unique_ptr<gfx::bake::BakedModel> Model() {
    gfx::bake::BakedModelBones bones;
    bones.sorted_bone_indices = {1, 0};
    return std::make_unique<gfx::bake::BakedModel>(
        gfx::bake::BakedModelInfo{}, std::move(bones),
        gfx::bake::BakedModelCubes<simd::Width::B128>{});
}

TEST(BakedModelJniTest, TransfersHandleOnlyAfterCompleteMapping) {
    FakeJni jni;
    auto handle = lib::bake::DeliverModel(
        &jni.env, Model(), reinterpret_cast<jshortArray>(1));
    ASSERT_TRUE(handle.ok()) << handle.status();
    EXPECT_EQ(jni.mapping, (std::array<jshort, 2>{1, 0}));
    auto acquired = java::AcquireOpaquePtr<gfx::bake::BakedModel>(*handle);
    ASSERT_TRUE(acquired.ok());
    EXPECT_EQ((*acquired)->Bones().sorted_bone_indices,
              (std::vector<uint16_t>{1, 0}));
    java::DestroyOpaquePtr(*handle);
    EXPECT_EQ(acquired->use_count(), 1);
}

TEST(BakedModelJniTest, RejectsPendingExceptionBeforeAndAfterMapping) {
    FakeJni pending;
    pending.pending = true;
    EXPECT_FALSE(lib::bake::DeliverModel(
        &pending.env, Model(), reinterpret_cast<jshortArray>(1)).ok());
    EXPECT_EQ(pending.mapping_writes, 0);
    FakeJni write_failure;
    write_failure.fail_mapping = true;
    EXPECT_FALSE(lib::bake::DeliverModel(
        &write_failure.env, Model(), reinterpret_cast<jshortArray>(1)).ok());
    EXPECT_EQ(write_failure.mapping_writes, 1);
    FakeJni null_mapping;
    EXPECT_FALSE(lib::bake::DeliverModel(&null_mapping.env, Model(), nullptr).ok());
}

TEST(BakedModelJniTest, DirectOutputRemainsNativeOwnedOnNullOrPendingException) {
    for (const auto null_output : {false, true}) {
        FakeJni failure;
        failure.null_output = null_output;
        failure.fail_output = true;
        auto output = java::TryMoveToOutput(
            &failure.env, BufferManaged(16), java::BufferType::kDirect);
        ASSERT_TRUE(output.ok());
        EXPECT_EQ(*output, nullptr);
        EXPECT_EQ(failure.deleted_refs, null_output ? 0 : 1);
    }
    FakeJni pending;
    pending.pending = true;
    EXPECT_EQ(*java::TryMoveToOutput(
        &pending.env, BufferManaged(16), java::BufferType::kDirect), nullptr);
    EXPECT_EQ(pending.output_memory, nullptr);

    FakeJni success;
    auto output = java::TryMoveToOutput(
        &success.env, BufferManaged(16), java::BufferType::kDirect);
    ASSERT_TRUE(output.ok());
    EXPECT_NE(*output, nullptr);
    BufferManaged adopted(BufferView(static_cast<Byte*>(success.output_memory), 16),
                          BufferManaged::OwnFlag{});
}
}  // namespace
}  // namespace ysm::test
