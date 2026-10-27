#pragma once
#include <optional>
#include <variant>
#include <ylt/struct_pb.hpp>

#include "proto/common/program.proto.h"

namespace ysm::proto::asset::model::data {

enum class EasingType {
    kNone = 0,
    kLinear = 1,
    kCatmullrom = 2,
};

enum class LoopType {
    kUnspecified = 0,
    kLoop = 1,
    kPlayOnce = 2,
    kHoldOnLastFrame = 3,
};

struct ExpressionValue : public iguana::base_impl<ExpressionValue> {
    ExpressionValue() = default;
    explicit ExpressionValue(
        std::variant<float, ::ysm::proto::common::Program> a)
        : value(std::move(a)) {}
    std::variant<float, ::ysm::proto::common::Program> value;
};
YLT_REFL(ExpressionValue, value);

struct BoneKeyFrame : public iguana::base_impl<BoneKeyFrame> {
    BoneKeyFrame() = default;
    BoneKeyFrame(float a, EasingType b, std::vector<ExpressionValue> c,
                 std::vector<ExpressionValue> d)
        : start_tick(a), easing_type(b), pre(std::move(c)), post(std::move(d)) {}
    float start_tick;
    EasingType easing_type;
    std::vector<ExpressionValue> pre;
    std::vector<ExpressionValue> post;
};
YLT_REFL(BoneKeyFrame, start_tick, easing_type, pre, post);

struct BoneAnimation : public iguana::base_impl<BoneAnimation> {
    BoneAnimation() = default;
    BoneAnimation(std::string a, std::vector<BoneKeyFrame> b,
                  std::vector<BoneKeyFrame> c, std::vector<BoneKeyFrame> d)
        : bone_name(std::move(a)), rotation(std::move(b)),
          position(std::move(c)), scale(std::move(d)) {}
    std::string bone_name;
    std::vector<BoneKeyFrame> rotation;
    std::vector<BoneKeyFrame> position;
    std::vector<BoneKeyFrame> scale;
};
YLT_REFL(BoneAnimation, bone_name, rotation, position, scale);

struct EventKeyFrame : public iguana::base_impl<EventKeyFrame> {
    EventKeyFrame() = default;
    EventKeyFrame(std::string a, float b) : data(std::move(a)), start_tick(b) {}
    std::string data;
    float start_tick;
};
YLT_REFL(EventKeyFrame, data, start_tick);

struct InstructionKeyFrame : public iguana::base_impl<InstructionKeyFrame> {
    InstructionKeyFrame() = default;
    InstructionKeyFrame(::ysm::proto::common::Program a, float b)
        : programs(std::move(a)), start_tick(b) {}
    ::ysm::proto::common::Program programs;
    float start_tick;
};
YLT_REFL(InstructionKeyFrame, programs, start_tick);

struct Animation : public iguana::base_impl<Animation> {
    Animation() = default;
    Animation(std::string a, float b, std::optional<LoopType> c,
              std::optional<ExpressionValue> d, std::vector<BoneAnimation> e,
              std::vector<InstructionKeyFrame> f,
              std::vector<EventKeyFrame> g)
        : name(std::move(a)), length(b), loop(c), blend_weight(std::move(d)),
          bone_animations(std::move(e)), instruction_keyframes(std::move(f)),
          sound_keyframes(std::move(g)) {}
    std::string name;
    float length;
    std::optional<LoopType> loop;
    std::optional<ExpressionValue> blend_weight;
    std::vector<BoneAnimation> bone_animations;
    std::vector<InstructionKeyFrame> instruction_keyframes;
    std::vector<EventKeyFrame> sound_keyframes;
};
YLT_REFL(Animation, name, length, loop, blend_weight, bone_animations,
         instruction_keyframes, sound_keyframes);

struct AnimationFile : public iguana::base_impl<AnimationFile> {
    AnimationFile() = default;
    explicit AnimationFile(std::vector<Animation> a)
        : animations(std::move(a)) {}
    std::vector<Animation> animations;
};
YLT_REFL(AnimationFile, animations);

}
