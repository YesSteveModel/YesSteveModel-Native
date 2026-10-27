#include "decoder.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <codec/audio.h>
#include <codec/mini_ogg.h>
#include <err.h>
#include <v3/codec/envelope.h>

#include <v3/container/image.h>

namespace ysm::legacy::v3::container {
namespace {
constexpr std::uint32_t kStringByteLimit = 16 * 1024 * 1024;
constexpr std::uint32_t kNonSoundByteLimit = 66 * 1024 * 1024;
constexpr std::uint32_t kSoundByteLimit = 4 * 1024 * 1024;
constexpr std::uint32_t kCollectionEntryLimit = 1'048'576;
constexpr std::uint32_t kBoneLimit = 65'536;
constexpr std::uint32_t kCubeFaceLimit = 6;

#define YSM_LEGACY_TRY(...)   \
    do {                      \
        if (!(__VA_ARGS__)) { \
            return false;     \
        }                     \
    } while (false)
#define YSM_LEGACY_INVALID(...) \
    return context.Fail(absl::DataLossError(__VA_ARGS__))
#define YSM_LEGACY_RESOURCE_LIMIT(...) \
    return context.Fail(absl::ResourceExhaustedError(__VA_ARGS__))
#define YSM_LEGACY_TARGET_REPRESENTATION(...) \
    return context.Fail(absl::FailedPreconditionError(__VA_ARGS__))
#define YSM_LEGACY_UNSUPPORTED(...) \
    return context.Fail(absl::UnimplementedError(__VA_ARGS__))

struct Budget {
    std::uint32_t collection_entries{};
};

template <typename ReaderType>
struct Context {
    explicit Context(ReaderType& reader) : reader(reader) {}

    bool ReadExact(BufferView destination) {
        if (reader.ReadExact(destination)) {
            return true;
        }
        status = reader.status();
        return false;
    }

    bool Fail(absl::Status failure) {
        status = std::move(failure);
        return false;
    }

    ReaderType& reader;
    Budget budget;
    std::uint32_t version{};
    std::uint32_t sound_field_count{};
    std::uint32_t omitted_sound_count{};
    absl::Status status;
};

bool ReadByte(auto& context, Byte& value) {
    YSM_LEGACY_TRY(context.ReadExact(BufferView(&value, 1)));
    return true;
}

bool ReadFixedU32(auto& context, std::uint32_t& value) {
    BufferFixed<4> bytes{};
    YSM_LEGACY_TRY(context.ReadExact(bytes));
    value = static_cast<std::uint32_t>(bytes[0]) |
            static_cast<std::uint32_t>(bytes[1]) << 8U |
            static_cast<std::uint32_t>(bytes[2]) << 16U |
            static_cast<std::uint32_t>(bytes[3]) << 24U;
    return true;
}

bool ReadVarU32(auto& context, std::uint32_t& value) {
    value = 0;
    for (std::uint32_t index = 0; index < 5; ++index) {
        Byte byte{};
        YSM_LEGACY_TRY(ReadByte(context, byte));
        if (index == 4 && (byte & 0xF0U) != 0) {
            YSM_LEGACY_INVALID("Historical uint32 varint overflows");
        }
        value |= static_cast<std::uint32_t>(byte & 0x7FU) << (index * 7U);
        if ((byte & 0x80U) == 0) {
            if (index != 0 && byte == 0) {
                YSM_LEGACY_INVALID("Historical uint32 varint is not canonical");
            }
            return true;
        }
    }
    YSM_LEGACY_INVALID("Historical uint32 varint is unterminated");
    return true;
}

bool ReadVarU64(auto& context, std::uint64_t& value) {
    value = 0;
    for (std::uint32_t index = 0; index < 10; ++index) {
        Byte byte{};
        YSM_LEGACY_TRY(ReadByte(context, byte));
        if (index == 9 && (byte & 0xFEU) != 0) {
            YSM_LEGACY_INVALID("Historical uint64 varint overflows");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << (index * 7U);
        if ((byte & 0x80U) == 0) {
            if (index != 0 && byte == 0) {
                YSM_LEGACY_INVALID("Historical uint64 varint is not canonical");
            }
            return true;
        }
    }
    YSM_LEGACY_INVALID("Historical uint64 varint is unterminated");
    return true;
}

bool ReadBool(auto& context, bool& value) {
    Byte byte{};
    YSM_LEGACY_TRY(ReadByte(context, byte));
    if (byte > 1) {
        YSM_LEGACY_INVALID("Historical boolean has an invalid value");
    }
    value = byte != 0;
    return true;
}

bool ReadRawFloat(auto& context, float& value) {
    std::uint32_t bits{};
    YSM_LEGACY_TRY(ReadFixedU32(context, bits));
    value = std::bit_cast<float>(bits);
    return true;
}

bool ReadFloat(auto& context, float& value,
               [[maybe_unused]] std::string field = "unspecified") {
    YSM_LEGACY_TRY(ReadRawFloat(context, value));
    return true;
}

bool ReadDouble(auto& context, double& value, std::string field = "float64") {
    float stored{};
    YSM_LEGACY_TRY(ReadFloat(context, stored, field));
    value = stored;
    return true;
}

bool ReadAnimationLength(auto& context, double& value,
                         [[maybe_unused]] std::string_view animation_name) {
    float stored{};
    YSM_LEGACY_TRY(ReadRawFloat(context, stored));
    value = stored;
    return true;
}

bool IsContinuation(Byte byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

bool IsValidUtf8(std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const Byte*>(value.data());
    std::size_t offset = 0;
    while (offset < value.size()) {
        const Byte first = bytes[offset++];
        if (first <= 0x7FU) {
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            if (offset >= value.size() || !IsContinuation(bytes[offset++])) {
                return false;
            }
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU) {
            if (offset + 1 >= value.size()) {
                return false;
            }
            const Byte second = bytes[offset++];
            const Byte third = bytes[offset++];
            if (!IsContinuation(second) || !IsContinuation(third) ||
                (first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U)) {
                return false;
            }
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U) {
            if (offset + 2 >= value.size()) {
                return false;
            }
            const Byte second = bytes[offset++];
            const Byte third = bytes[offset++];
            const Byte fourth = bytes[offset++];
            if (!IsContinuation(second) || !IsContinuation(third) ||
                !IsContinuation(fourth) || (first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second >= 0x90U)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool ReadStringBytes(auto& context, std::string& value) {
    std::uint32_t size{};
    YSM_LEGACY_TRY(ReadVarU32(context, size));
    if (size > kStringByteLimit) {
        YSM_LEGACY_RESOURCE_LIMIT("Historical string exceeds its byte limit");
    }
    value.resize(size);
    YSM_LEGACY_TRY(context.ReadExact(
        {reinterpret_cast<Byte*>(value.data()), value.size()}));
    return true;
}

bool ReadString(auto& context, std::string& value) {
    YSM_LEGACY_TRY(ReadStringBytes(context, value));
    if (!IsValidUtf8(value)) {
        YSM_LEGACY_TARGET_REPRESENTATION(
            "Historical string is not representable as target UTF-8");
    }
    return true;
}

bool ReadBytes(auto& context, BufferManaged& value, std::uint32_t limit,
               std::string_view field) {
    std::uint32_t size{};
    YSM_LEGACY_TRY(ReadVarU32(context, size));
    if (size > limit) {
        YSM_LEGACY_RESOURCE_LIMIT("Historical " + std::string(field) + " has " +
                                  std::to_string(size) +
                                  " bytes, exceeding its limit");
    }
    value.resize(size);
    YSM_LEGACY_TRY(context.ReadExact(value));
    return true;
}

bool ReadCount(auto& context, std::uint32_t& count,
               std::uint32_t field_limit = kCollectionEntryLimit) {
    YSM_LEGACY_TRY(ReadVarU32(context, count));
    if (count > field_limit) {
        YSM_LEGACY_RESOURCE_LIMIT(
            "Historical collection exceeds its field limit");
    }
    if (count > kCollectionEntryLimit - context.budget.collection_entries) {
        YSM_LEGACY_RESOURCE_LIMIT(
            "Historical collection aggregate exceeds its limit");
    }
    context.budget.collection_entries += count;
    return true;
}

template <typename Value, typename ReadValue>
bool ReadVector(auto& context, std::vector<Value>& values, ReadValue read_value,
                std::uint32_t field_limit = kCollectionEntryLimit) {
    std::uint32_t count{};
    YSM_LEGACY_TRY(ReadCount(context, count, field_limit));
    values.clear();
    for (std::uint32_t index = 0; index < count; ++index) {
        Value value;
        YSM_LEGACY_TRY(read_value(value));
        values.emplace_back(std::move(value));
    }
    return true;
}

template <typename Key, typename Value, typename KeyReader,
          typename ValueReader>
bool ReadMap(auto& context, OrderedMap<Key, Value>& values, KeyReader read_key,
             ValueReader read_value) {
    std::uint32_t count{};
    YSM_LEGACY_TRY(ReadCount(context, count));
    values.clear();
    std::unordered_set<Key> keys;
    for (std::uint32_t index = 0; index < count; ++index) {
        Key key;
        YSM_LEGACY_TRY(read_key(key));
        if (!keys.emplace(key).second) {
            YSM_LEGACY_TARGET_REPRESENTATION(
                "Historical map contains a duplicate target key");
        }
        Value value;
        YSM_LEGACY_TRY(read_value(value));
        values.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

template <typename Value, typename ReadValue>
bool ReadOptional(auto& context, std::optional<Value>& value,
                  ReadValue read_value) {
    std::uint32_t tag{};
    YSM_LEGACY_TRY(ReadVarU32(context, tag));
    if (tag > 1) {
        YSM_LEGACY_INVALID("Historical optional tag has an invalid value");
    }
    if (tag == 0) {
        value.reset();
        return true;
    }
    value.emplace();
    YSM_LEGACY_TRY(read_value(*value));
    return true;
}

template <typename Enum, typename Predicate>
bool ReadEnum(auto& context, Enum& value, Predicate valid) {
    std::uint32_t raw{};
    YSM_LEGACY_TRY(ReadVarU32(context, raw));
    if (!valid(raw)) {
        YSM_LEGACY_INVALID("Historical enum has an unknown value");
    }
    value = static_cast<Enum>(raw);
    return true;
}

bool ReadStringVector(auto& context, std::vector<std::string>& values) {
    YSM_LEGACY_TRY(ReadVector(context, values, [&](std::string& value) -> bool {
        YSM_LEGACY_TRY(ReadString(context, value));
        return true;
    }));
    return true;
}

bool ReadStringMap(auto& context,
                   OrderedMap<std::string, std::string>& values) {
    YSM_LEGACY_TRY(ReadMap(
        context, values,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](std::string& value) -> bool {
            YSM_LEGACY_TRY(ReadString(context, value));
            return true;
        }));
    return true;
}

bool ReadStringSourceHashMap(auto& context,
                             OrderedMap<std::string, std::string>& values) {
    YSM_LEGACY_TRY(ReadMap(
        context, values,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](std::string& value) -> bool {
            YSM_LEGACY_TRY(ReadStringBytes(context, value));
            return true;
        }));
    return true;
}

bool ParseVec3(auto& context, Vec3& value) {
    YSM_LEGACY_TRY(ReadFloat(context, value.x, "vector x"));
    YSM_LEGACY_TRY(ReadFloat(context, value.y, "vector y"));
    YSM_LEGACY_TRY(ReadFloat(context, value.z, "vector z"));
    return true;
}

bool ParseVertex(auto& context, Vertex& value) {
    YSM_LEGACY_TRY(ParseVec3(context, value.position));
    YSM_LEGACY_TRY(ReadFloat(context, value.texture_u, "vertex texture u"));
    YSM_LEGACY_TRY(ReadFloat(context, value.texture_v, "vertex texture v"));
    return true;
}

bool ParseQuad(auto& context, Quad& value) {
    YSM_LEGACY_TRY(ParseVec3(context, value.normal));
    for (auto& vertex : value.vertices) {
        YSM_LEGACY_TRY(ParseVertex(context, vertex));
    }
    return true;
}

bool ParseCube(auto& context, Cube& value) {
    YSM_LEGACY_TRY(ReadVector(
        context, value.quads,
        [&](Quad& quad) -> bool {
            YSM_LEGACY_TRY(ParseQuad(context, quad));
            return true;
        },
        kCubeFaceLimit));
    for (auto& zero_size : value.zero_size) {
        YSM_LEGACY_TRY(ReadBool(context, zero_size));
    }
    return true;
}

bool ParseExtraInfo(auto& context, ExtraInfo& value) {
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadString(context, value.tips));
    YSM_LEGACY_TRY(ReadStringVector(context, value.extra_animation_names));
    YSM_LEGACY_TRY(ReadStringVector(context, value.authors));
    YSM_LEGACY_TRY(ReadString(context, value.license));
    YSM_LEGACY_TRY(ReadBool(context, value.free));
    return true;
}

bool ParseModelScript(auto& context, ModelScript& value) {
    YSM_LEGACY_TRY(ReadStringMap(context, value.variables));
    YSM_LEGACY_TRY(ReadStringVector(context, value.initialize));
    YSM_LEGACY_TRY(ReadStringVector(context, value.pre_animation));
    return true;
}

bool ParseGeoProperties(auto& context, GeoProperties& value) {
    YSM_LEGACY_TRY(ReadString(context, value.identifier));
    YSM_LEGACY_TRY(
        ReadDouble(context, value.texture_height, "geo texture height"));
    YSM_LEGACY_TRY(
        ReadDouble(context, value.texture_width, "geo texture width"));
    YSM_LEGACY_TRY(ReadDouble(context, value.visible_bounds_height,
                              "geo visible bounds height"));
    YSM_LEGACY_TRY(ReadDouble(context, value.visible_bounds_width,
                              "geo visible bounds width"));
    YSM_LEGACY_TRY(ReadVector(
        context, value.visible_bounds_offset, [&](double& item) -> bool {
            YSM_LEGACY_TRY(
                ReadDouble(context, item, "geo visible bounds offset"));
            return true;
        }));
    YSM_LEGACY_TRY(ReadDouble(context, value.height_scale, "geo height scale"));
    YSM_LEGACY_TRY(ReadDouble(context, value.width_scale, "geo width scale"));
    YSM_LEGACY_TRY(
        ReadOptional(context, value.extra_info, [&](ExtraInfo& item) -> bool {
            YSM_LEGACY_TRY(ParseExtraInfo(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ParseModelScript(context, value.script));
    return true;
}

bool ParseBone(auto& context, Bone& value) {
    YSM_LEGACY_TRY(ReadString(context, value.parent));
    if (context.version == 5) {
        std::vector<Quad> quads;
        YSM_LEGACY_TRY(ReadVector(context, quads, [&](Quad& quad) -> bool {
            YSM_LEGACY_TRY(ParseQuad(context, quad));
            return true;
        }));
        for (auto& quad : quads) {
            Cube cube;
            cube.quads.emplace_back(std::move(quad));
            value.cubes.emplace_back(std::move(cube));
        }
    } else {
        YSM_LEGACY_TRY(
            ReadVector(context, value.cubes, [&](Cube& cube) -> bool {
                YSM_LEGACY_TRY(ParseCube(context, cube));
                return true;
            }));
    }
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadBool(context, value.dont_render));
    YSM_LEGACY_TRY(ReadBool(context, value.hidden));
    YSM_LEGACY_TRY(ReadBool(context, value.cubes_hidden));
    YSM_LEGACY_TRY(ReadBool(context, value.hide_children));
    YSM_LEGACY_TRY(ReadBool(context, value.reset));
    YSM_LEGACY_TRY(ParseVec3(context, value.pivot));
    YSM_LEGACY_TRY(ParseVec3(context, value.rotation));
    return true;
}

bool ParseGeoModel(auto& context, GeoModel& value) {
    YSM_LEGACY_TRY(ReadVector(
        context, value.bones,
        [&](Bone& bone) -> bool {
            YSM_LEGACY_TRY(ParseBone(context, bone));
            return true;
        },
        kBoneLimit));
    YSM_LEGACY_TRY(ParseGeoProperties(context, value.properties));
    if (context.version == 5) {
        value.v5_global_cube_count.emplace();
        YSM_LEGACY_TRY(ReadVarU32(context, *value.v5_global_cube_count));
    }
    return true;
}

bool ParseMolangValue(auto& context, MolangValue& value) {
    YSM_LEGACY_TRY(ReadEnum(context, value.type, [](std::uint32_t raw) {
        return raw <= static_cast<std::uint32_t>(MolangValueType::kString);
    }));
    if (value.type == MolangValueType::kString) {
        YSM_LEGACY_TRY(ReadString(context, value.string_value));
    } else if (value.type == MolangValueType::kDouble) {
        YSM_LEGACY_TRY(
            ReadDouble(context, value.double_value, "molang number"));
    }
    return true;
}

bool ParseMolangVec3(auto& context, MolangVec3& value) {
    YSM_LEGACY_TRY(ParseMolangValue(context, value.x));
    YSM_LEGACY_TRY(ParseMolangValue(context, value.y));
    YSM_LEGACY_TRY(ParseMolangValue(context, value.z));
    return true;
}

bool ParseBoneKeyFrame(auto& context, BoneKeyFrame& value) {
    YSM_LEGACY_TRY(ReadDouble(context, value.start_tick, "bone keyframe tick"));
    YSM_LEGACY_TRY(
        ReadEnum(context, value.easing, [](std::uint32_t) { return true; }));
    YSM_LEGACY_TRY(ParseMolangVec3(context, value.pre));
    YSM_LEGACY_TRY(
        ReadOptional(context, value.post, [&](MolangVec3& item) -> bool {
            YSM_LEGACY_TRY(ParseMolangVec3(context, item));
            return true;
        }));
    return true;
}

bool ParseBoneAnimation(auto& context, BoneAnimation& value) {
    YSM_LEGACY_TRY(ReadString(context, value.bone_name));
    const auto read_frame = [&](BoneKeyFrame& frame) -> bool {
        YSM_LEGACY_TRY(ParseBoneKeyFrame(context, frame));
        return true;
    };
    YSM_LEGACY_TRY(ReadVector(context, value.rotations, read_frame));
    YSM_LEGACY_TRY(ReadVector(context, value.positions, read_frame));
    YSM_LEGACY_TRY(ReadVector(context, value.scales, read_frame));
    return true;
}

bool ParseEventKeyFrame(auto& context, EventKeyFrame& value) {
    YSM_LEGACY_TRY(ReadString(context, value.data));
    YSM_LEGACY_TRY(
        ReadDouble(context, value.start_tick, "event keyframe tick"));
    return true;
}

bool ParseInstructionKeyFrame(auto& context, InstructionKeyFrame& value) {
    YSM_LEGACY_TRY(ReadStringVector(context, value.data));
    YSM_LEGACY_TRY(
        ReadDouble(context, value.start_tick, "instruction keyframe tick"));
    return true;
}

bool ParseAnimation(auto& context, Animation& value) {
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadAnimationLength(context, value.length, value.name));
    YSM_LEGACY_TRY(
        ReadEnum(context, value.loop, [](std::uint32_t) { return true; }));
    if (context.version >= 10) {
        const auto read_molang = [&](MolangValue& item) -> bool {
            YSM_LEGACY_TRY(ParseMolangValue(context, item));
            return true;
        };
        YSM_LEGACY_TRY(ReadOptional(context, value.start_delay, read_molang));
        YSM_LEGACY_TRY(ReadOptional(context, value.loop_delay, read_molang));
        YSM_LEGACY_TRY(ReadOptional(context, value.blend_weight, read_molang));
        YSM_LEGACY_TRY(ReadOptional(context, value.override_previous,
                                    [&](bool& item) -> bool {
                                        YSM_LEGACY_TRY(ReadBool(context, item));
                                        return true;
                                    }));
    }
    YSM_LEGACY_TRY(
        ReadVector(context, value.bones, [&](BoneAnimation& item) -> bool {
            YSM_LEGACY_TRY(ParseBoneAnimation(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ReadVector(
        context, value.instructions, [&](InstructionKeyFrame& item) -> bool {
            YSM_LEGACY_TRY(ParseInstructionKeyFrame(context, item));
            return true;
        }));
    if (context.version >= 11) {
        YSM_LEGACY_TRY(
            ReadVector(context, value.sounds, [&](EventKeyFrame& item) -> bool {
                YSM_LEGACY_TRY(ParseEventKeyFrame(context, item));
                return true;
            }));
    }
    return true;
}

bool ParseAnimationFile(auto& context, AnimationFile& value) {
    YSM_LEGACY_TRY(
        ReadVector(context, value.animations, [&](Animation& item) -> bool {
            YSM_LEGACY_TRY(ParseAnimation(context, item));
            return true;
        }));
    return true;
}

bool ParseAnimationEntry(auto& context, AnimationEntry& value) {
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadString(context, value.condition));
    return true;
}

bool ParseTransition(auto& context, Transition& value) {
    YSM_LEGACY_TRY(ReadString(context, value.destination));
    YSM_LEGACY_TRY(ReadString(context, value.condition));
    return true;
}

bool ParseBlendTransition(auto& context, BlendTransition& value) {
    if (context.version < 15) {
        value.linear_length.emplace();
        YSM_LEGACY_TRY(ReadFloat(context, *value.linear_length,
                                 "controller transition length"));
        return true;
    }
    YSM_LEGACY_TRY(
        ReadOptional(context, value.linear_length, [&](float& item) -> bool {
            YSM_LEGACY_TRY(
                ReadFloat(context, item, "controller transition length"));
            return true;
        }));
    if (!value.linear_length) {
        YSM_LEGACY_TRY(ReadMap(
            context, value.points,
            [&](float& key) -> bool {
                YSM_LEGACY_TRY(
                    ReadFloat(context, key, "controller blend point key"));
                return true;
            },
            [&](float& item) -> bool {
                YSM_LEGACY_TRY(
                    ReadFloat(context, item, "controller blend point value"));
                return true;
            }));
    }
    return true;
}

bool ParseControllerState(auto& context, ControllerState& value) {
    YSM_LEGACY_TRY(ReadVector(
        context, value.animations, [&](AnimationEntry& item) -> bool {
            YSM_LEGACY_TRY(ParseAnimationEntry(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(
        ReadVector(context, value.transitions, [&](Transition& item) -> bool {
            YSM_LEGACY_TRY(ParseTransition(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ReadStringVector(context, value.on_entry));
    YSM_LEGACY_TRY(ReadStringVector(context, value.on_exit));
    YSM_LEGACY_TRY(ParseBlendTransition(context, value.blend_transition));
    YSM_LEGACY_TRY(ReadBool(context, value.blend_via_shortest_path));
    if (context.version >= 28) {
        YSM_LEGACY_TRY(ReadVector(context, value.sound_effects,
                                  [&](std::string& item) -> bool {
                                      YSM_LEGACY_TRY(ReadString(context, item));
                                      return true;
                                  }));
    }
    return true;
}

bool ParseAnimationController(auto& context, AnimationController& value) {
    YSM_LEGACY_TRY(ReadString(context, value.initial_state));
    YSM_LEGACY_TRY(ReadMap(
        context, value.states,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](ControllerState& item) -> bool {
            YSM_LEGACY_TRY(ParseControllerState(context, item));
            return true;
        }));
    return true;
}

bool ParseAnimationControllerFile(auto& context,
                                  AnimationControllerFile& value) {
    YSM_LEGACY_TRY(ReadMap(
        context, value.controllers,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](AnimationController& item) -> bool {
            YSM_LEGACY_TRY(ParseAnimationController(context, item));
            return true;
        }));
    return true;
}

enum class SoundDisposition : std::uint8_t {
    kPlayable,
    kUnknown,
    kCorruptSupported,
};

struct SoundClassification {
    SoundDisposition disposition{SoundDisposition::kUnknown};
    SoundEncoding encoding{SoundEncoding::kVorbis};
    std::uint32_t channels{};
    std::uint32_t sample_rate{};
    std::uint64_t samples{};
    std::string diagnostic;
};

bool FirstOggPacketHasPrefix(BufferViewR bytes, BufferViewR prefix) {
    constexpr BufferFixed<4> kOgg{'O', 'g', 'g', 'S'};
    if (bytes.size() < ::ysm::codec::kOggPageHeaderSize ||
        !Cmp(bytes.first<kOgg.size()>(), kOgg)) {
        return false;
    }
    const auto segment_count = static_cast<std::size_t>(bytes[26]);
    const auto body_offset = ::ysm::codec::kOggPageHeaderSize + segment_count;
    if (body_offset > bytes.size()) {
        return false;
    }
    std::size_t packet_size = 0;
    for (std::size_t index = 0; index < segment_count; ++index) {
        const auto size = static_cast<std::size_t>(bytes[27 + index]);
        if (size > bytes.size() - body_offset - packet_size) {
            return false;
        }
        packet_size += size;
        if (size != 255) {
            break;
        }
    }
    return packet_size >= prefix.size() &&
           Cmp(bytes.subspan(body_offset, prefix.size()), prefix);
}

SoundClassification DetectSound(BufferViewR bytes) {
    constexpr BufferFixed<4> kOgg{'O', 'g', 'g', 'S'};
    constexpr BufferFixed<8> kOpus{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    constexpr BufferFixed<7> kVorbis{1, 'v', 'o', 'r', 'b', 'i', 's'};
    if (bytes.size() < kOgg.size() ||
        !Cmp(Slice(bytes, 0, kOgg.size()), kOgg)) {
        return {
            SoundDisposition::kUnknown,        SoundEncoding::kVorbis, 0, 0, 0,
            "no Ogg capture pattern was found"};
    }

    const bool looks_opus = FirstOggPacketHasPrefix(bytes, kOpus);
    const bool looks_vorbis = FirstOggPacketHasPrefix(bytes, kVorbis);
    const auto facts = ::ysm::codec::InspectAudio(bytes);
    if (!facts.ok()) {
        return {looks_opus || looks_vorbis ? SoundDisposition::kCorruptSupported
                                           : SoundDisposition::kUnknown,
                looks_opus ? SoundEncoding::kOpus : SoundEncoding::kVorbis,
                0,
                0,
                0,
                looks_opus || looks_vorbis
                    ? std::string(facts.status().message())
                    : "no supported Vorbis or Opus stream was found"};
    }
    return {SoundDisposition::kPlayable,
            facts->codec == ::ysm::codec::AudioCodec::kOpus
                ? SoundEncoding::kOpus
                : SoundEncoding::kVorbis,
            facts->channels,
            facts->sample_rate,
            facts->frames,
            {}};
}

bool ParseSound(auto& context, LegacySound& value, bool& playable) {
    YSM_LEGACY_TRY(
        ReadBytes(context, value.bytes, kSoundByteLimit, "sound payload"));
    ++context.sound_field_count;
    const auto classification = DetectSound(value.bytes);
    switch (classification.disposition) {
        case SoundDisposition::kPlayable:
            value.encoding = classification.encoding;
            value.channels = classification.channels;
            value.sample_rate = classification.sample_rate;
            value.samples = classification.samples;
            playable = true;
            return true;
        case SoundDisposition::kUnknown:
            ++context.omitted_sound_count;
            playable = false;
            return true;
        case SoundDisposition::kCorruptSupported:
            YSM_LEGACY_INVALID("Historical supported Ogg sound is corrupt: " +
                               std::string(classification.diagnostic) + " (" +
                               std::to_string(value.bytes.size()) + " bytes)");
    }
    return true;
}

bool ReadCommonSoundMap(
    auto& context,
    OrderedMap<std::string, WithSourceHash<LegacySound>>& values) {
    std::uint32_t count{};
    YSM_LEGACY_TRY(ReadCount(context, count));
    values.clear();
    std::unordered_set<std::string> keys;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string key;
        YSM_LEGACY_TRY(ReadString(context, key));
        WithSourceHash<LegacySound> item;
        YSM_LEGACY_TRY(ReadStringBytes(context, item.source_hash));
        bool playable{};
        YSM_LEGACY_TRY(ParseSound(context, item.value, playable));
        if (playable) {
            if (!keys.emplace(key).second) {
                YSM_LEGACY_TARGET_REPRESENTATION(
                    "Historical emitted sound contains a duplicate name");
            }
            values.emplace_back(std::move(key), std::move(item));
        }
    }
    return true;
}

bool ReadLegacySoundMap(auto& context,
                        OrderedMap<std::string, LegacySound>& values) {
    std::uint32_t count{};
    YSM_LEGACY_TRY(ReadCount(context, count));
    values.clear();
    std::unordered_set<std::string> keys;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string key;
        YSM_LEGACY_TRY(ReadString(context, key));
        LegacySound sound;
        bool playable{};
        YSM_LEGACY_TRY(ParseSound(context, sound, playable));
        if (playable) {
            if (!keys.emplace(key).second) {
                YSM_LEGACY_TARGET_REPRESENTATION(
                    "Historical emitted sound contains a duplicate name");
            }
            values.emplace_back(std::move(key), std::move(sound));
        }
    }
    return true;
}

bool ParseImage(auto& context, LegacyImage& value, bool legacy_png,
                ImageRole role) {
    YSM_LEGACY_TRY(
        ReadBytes(context, value.bytes, kNonSoundByteLimit, "image payload"));
    YSM_LEGACY_TRY(ReadVarU32(context, value.width));
    YSM_LEGACY_TRY(ReadVarU32(context, value.height));
    if (!legacy_png && context.version >= 23) {
        YSM_LEGACY_TRY(ReadEnum(context, value.encoding, [](std::uint32_t raw) {
            return raw >= static_cast<std::uint32_t>(ImageEncoding::kRgba) &&
                   raw <= static_cast<std::uint32_t>(ImageEncoding::kAvif);
        }));
        YSM_LEGACY_TRY(ReadVarU32(context, value.frame_count));
    } else {
        value.encoding = ImageEncoding::kRgba;
        value.frame_count = 1;
    }
    if (auto status = PrepareImage(value, role); !status.ok()) {
        return context.Fail(std::move(status));
    }
    return true;
}

bool ParsePbrType(auto& context, PbrType& value) {
    YSM_LEGACY_TRY(
        ReadEnum(context, value, [](std::uint32_t) { return true; }));
    return true;
}

template <typename Value, typename ReadValue>
bool ParseWithSourceHash(auto& context, WithSourceHash<Value>& value,
                         ReadValue read_value) {
    YSM_LEGACY_TRY(ReadStringBytes(context, value.source_hash));
    YSM_LEGACY_TRY(read_value(value.value));
    return true;
}

bool ParseTextureSet(auto& context, TextureSet& value) {
    YSM_LEGACY_TRY(
        ParseWithSourceHash(context, value.uv, [&](LegacyImage& item) -> bool {
            YSM_LEGACY_TRY(
                ParseImage(context, item, false, ImageRole::kTexture));
            return true;
        }));
    YSM_LEGACY_TRY(ReadMap(
        context, value.pbr,
        [&](PbrType& key) -> bool {
            YSM_LEGACY_TRY(ParsePbrType(context, key));
            return true;
        },
        [&](WithSourceHash<LegacyImage>& item) -> bool {
            YSM_LEGACY_TRY(ParseWithSourceHash(
                context, item, [&](LegacyImage& image) -> bool {
                    YSM_LEGACY_TRY(
                        ParseImage(context, image, false, ImageRole::kTexture));
                    return true;
                }));
            return true;
        }));
    return true;
}

bool ParseModelAuthor(auto& context, ModelAuthor& value) {
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadString(context, value.role));
    YSM_LEGACY_TRY(ReadStringMap(context, value.contact));
    YSM_LEGACY_TRY(ReadString(context, value.comment));
    return true;
}

bool ParseMetadata(auto& context, ModelMetadata& value) {
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadString(context, value.tips));
    YSM_LEGACY_TRY(ReadString(context, value.license.type));
    YSM_LEGACY_TRY(ReadString(context, value.license.description));
    YSM_LEGACY_TRY(
        ReadVector(context, value.authors, [&](ModelAuthor& item) -> bool {
            YSM_LEGACY_TRY(ParseModelAuthor(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ReadStringMap(context, value.links));
    return true;
}

bool ParseConfigForm(auto& context, ConfigForm& value) {
    YSM_LEGACY_TRY(ReadString(context, value.type));
    YSM_LEGACY_TRY(ReadString(context, value.title));
    YSM_LEGACY_TRY(ReadString(context, value.description));
    YSM_LEGACY_TRY(ReadString(context, value.value));
    YSM_LEGACY_TRY(ReadDouble(context, value.step, "config step"));
    YSM_LEGACY_TRY(ReadDouble(context, value.minimum, "config minimum"));
    YSM_LEGACY_TRY(ReadDouble(context, value.maximum, "config maximum"));
    YSM_LEGACY_TRY(ReadStringMap(context, value.labels));
    return true;
}

bool ParseExtraAnimationButton(auto& context, ExtraAnimationButton& value) {
    YSM_LEGACY_TRY(ReadString(context, value.id));
    YSM_LEGACY_TRY(ReadString(context, value.name));
    YSM_LEGACY_TRY(ReadString(context, value.sound));
    YSM_LEGACY_TRY(
        ReadVector(context, value.forms, [&](ConfigForm& item) -> bool {
            YSM_LEGACY_TRY(ParseConfigForm(context, item));
            return true;
        }));
    return true;
}

bool ParseExtraAnimationClassify(auto& context, ExtraAnimationClassify& value) {
    YSM_LEGACY_TRY(ReadString(context, value.id));
    YSM_LEGACY_TRY(ReadStringMap(context, value.animations));
    return true;
}

bool ParseModelProperties(auto& context, ModelProperties& value) {
    YSM_LEGACY_TRY(
        ReadFloat(context, value.height_scale, "model height scale"));
    YSM_LEGACY_TRY(ReadFloat(context, value.width_scale, "model width scale"));
    YSM_LEGACY_TRY(ReadStringMap(context, value.extra_animation));
    if (context.version >= 12) {
        YSM_LEGACY_TRY(ReadVector(context, value.extra_animation_buttons,
                                  [&](ExtraAnimationButton& item) -> bool {
                                      YSM_LEGACY_TRY(ParseExtraAnimationButton(
                                          context, item));
                                      return true;
                                  }));
        YSM_LEGACY_TRY(ReadVector(
            context, value.extra_animation_classify,
            [&](ExtraAnimationClassify& item) -> bool {
                YSM_LEGACY_TRY(ParseExtraAnimationClassify(context, item));
                return true;
            }));
    }
    YSM_LEGACY_TRY(ReadString(context, value.default_texture));
    YSM_LEGACY_TRY(ReadString(context, value.preview_animation));
    YSM_LEGACY_TRY(ReadBool(context, value.free));
    if (context.version >= 8) {
        YSM_LEGACY_TRY(ReadBool(context, value.render_layers_first));
    }
    if (context.version >= 13) {
        YSM_LEGACY_TRY(ReadBool(context, value.all_cutout));
    }
    if (context.version >= 14) {
        YSM_LEGACY_TRY(ReadBool(context, value.disable_preview_rotation));
    }
    if (context.version >= 17) {
        YSM_LEGACY_TRY(ReadBool(context, value.gui_no_lighting));
    }
    if (context.version >= 32) {
        YSM_LEGACY_TRY(ReadBool(context, value.merge_multiline_expr));
    }
    if (context.version >= 20) {
        YSM_LEGACY_TRY(ReadString(context, value.gui_foreground));
        YSM_LEGACY_TRY(ReadString(context, value.gui_background));
    }
    return true;
}

bool ParseExportInfo(auto& context, ExportInfo& value) {
    YSM_LEGACY_TRY(ReadString(context, value.random));
    YSM_LEGACY_TRY(ReadVarU64(context, value.timestamp));
    YSM_LEGACY_TRY(ReadString(context, value.extra));
    return true;
}

bool ParseLanguageFile(auto& context, LanguageFile& value) {
    YSM_LEGACY_TRY(ReadStringMap(context, value.entries));
    return true;
}

bool ParseCommonAssets(auto& context, CommonAssets& value) {
    YSM_LEGACY_TRY(ReadCommonSoundMap(context, value.sounds));
    YSM_LEGACY_TRY(ReadMap(
        context, value.user_functions,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](WithSourceHash<std::string>& item) -> bool {
            YSM_LEGACY_TRY(ParseWithSourceHash(
                context, item, [&](std::string& text) -> bool {
                    YSM_LEGACY_TRY(ReadString(context, text));
                    return true;
                }));
            return true;
        }));
    YSM_LEGACY_TRY(ReadMap(
        context, value.languages,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](WithSourceHash<LanguageFile>& item) -> bool {
            YSM_LEGACY_TRY(ParseWithSourceHash(
                context, item, [&](LanguageFile& language) -> bool {
                    YSM_LEGACY_TRY(ParseLanguageFile(context, language));
                    return true;
                }));
            return true;
        }));
    return true;
}

bool ParsePlayerModelType(auto& context, PlayerModelType& value) {
    YSM_LEGACY_TRY(
        ReadEnum(context, value, [](std::uint32_t) { return true; }));
    return true;
}

bool IsPlayerAnimationType(std::uint32_t raw) {
    return (raw >= 1 && raw <= 4) || (raw >= 6 && raw <= 13);
}

bool ParsePlayerAnimationType(auto& context, PlayerAnimationType& value) {
    YSM_LEGACY_TRY(
        ReadEnum(context, value, [](std::uint32_t) { return true; }));
    return true;
}

bool ParsePlayerModel(auto& context, PlayerModel& value) {
    YSM_LEGACY_TRY(ReadMap(
        context, value.animations,
        [&](PlayerAnimationType& key) -> bool {
            YSM_LEGACY_TRY(ParsePlayerAnimationType(context, key));
            return true;
        },
        [&](WithSourceHash<AnimationFile>& item) -> bool {
            YSM_LEGACY_TRY(ParseWithSourceHash(
                context, item, [&](AnimationFile& animation) -> bool {
                    YSM_LEGACY_TRY(ParseAnimationFile(context, animation));
                    return true;
                }));
            return true;
        }));
    YSM_LEGACY_TRY(ReadMap(
        context, value.controllers,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](WithSourceHash<AnimationControllerFile>& item) -> bool {
            YSM_LEGACY_TRY(ParseWithSourceHash(
                context, item,
                [&](AnimationControllerFile& controller) -> bool {
                    YSM_LEGACY_TRY(
                        ParseAnimationControllerFile(context, controller));
                    return true;
                }));
            return true;
        }));
    YSM_LEGACY_TRY(ReadMap(
        context, value.textures,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](TextureSet& item) -> bool {
            YSM_LEGACY_TRY(ParseTextureSet(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ReadMap(
        context, value.geo_models,
        [&](PlayerModelType& key) -> bool {
            YSM_LEGACY_TRY(ParsePlayerModelType(context, key));
            return true;
        },
        [&](WithSourceHash<GeoModel>& item) -> bool {
            YSM_LEGACY_TRY(
                ParseWithSourceHash(context, item, [&](GeoModel& geo) -> bool {
                    YSM_LEGACY_TRY(ParseGeoModel(context, geo));
                    return true;
                }));
            return true;
        }));
    return true;
}

bool ParseReplaceModel(auto& context, ReplaceModel& value) {
    YSM_LEGACY_TRY(ReadOptional(
        context, value.animation,
        [&](WithSourceHash<AnimationFile>& item) -> bool {
            YSM_LEGACY_TRY(ParseWithSourceHash(
                context, item, [&](AnimationFile& animation) -> bool {
                    YSM_LEGACY_TRY(ParseAnimationFile(context, animation));
                    return true;
                }));
            return true;
        }));
    if (context.version >= 22) {
        YSM_LEGACY_TRY(ReadOptional(
            context, value.controller,
            [&](WithSourceHash<AnimationControllerFile>& item) -> bool {
                YSM_LEGACY_TRY(ParseWithSourceHash(
                    context, item,
                    [&](AnimationControllerFile& controller) -> bool {
                        YSM_LEGACY_TRY(
                            ParseAnimationControllerFile(context, controller));
                        return true;
                    }));
                return true;
            }));
    }
    value.texture.emplace();
    YSM_LEGACY_TRY(ParseTextureSet(context, *value.texture));
    value.geo.emplace();
    YSM_LEGACY_TRY(
        ParseWithSourceHash(context, *value.geo, [&](GeoModel& geo) -> bool {
            YSM_LEGACY_TRY(ParseGeoModel(context, geo));
            return true;
        }));
    if (context.version >= 27) {
        YSM_LEGACY_TRY(ReadStringVector(context, value.match));
    }
    return true;
}

bool ParseModelInfo(auto& context, ModelInfo& value) {
    YSM_LEGACY_TRY(ReadStringBytes(context, value.hash));
    YSM_LEGACY_TRY(
        ReadOptional(context, value.metadata, [&](ModelMetadata& item) -> bool {
            YSM_LEGACY_TRY(ParseMetadata(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ParseModelProperties(context, value.properties));
    YSM_LEGACY_TRY(ReadMap(
        context, value.author_avatars,
        [&](std::string& key) -> bool {
            YSM_LEGACY_TRY(ReadString(context, key));
            return true;
        },
        [&](LegacyImage& image) -> bool {
            YSM_LEGACY_TRY(
                ParseImage(context, image, false, ImageRole::kAvatar));
            return true;
        }));
    if (context.version >= 20) {
        ImageRole gui_role{ImageRole::kGui};
        YSM_LEGACY_TRY(ReadMap(
            context, value.gui_images,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                if (key == "gui_foreground" || key == "gui_background") {
                    gui_role = ImageRole::kGui;
                } else if (key == "thumb-button") {
                    gui_role = ImageRole::kThumbnail;
                } else if (key == "thumb-icon") {
                    gui_role = ImageRole::kIcon;
                } else {
                    YSM_LEGACY_TARGET_REPRESENTATION(
                        "GUI image has no target role: " + key);
                }
                return true;
            },
            [&](LegacyImage& image) -> bool {
                YSM_LEGACY_TRY(ParseImage(context, image, false, gui_role));
                return true;
            }));
    }
    if (context.version >= 27) {
        YSM_LEGACY_TRY(ReadVarU32(context, value.origin_version));
    }
    return true;
}

bool ParseCurrentFamily(auto& context, LegacyModel& value) {
    YSM_LEGACY_TRY(ParseCommonAssets(context, value.common));
    if (context.version >= 27) {
        YSM_LEGACY_TRY(ReadVector(
            context, value.vehicles, [&](ReplaceModel& item) -> bool {
                YSM_LEGACY_TRY(ParseReplaceModel(context, item));
                return true;
            }));
        YSM_LEGACY_TRY(ReadVector(
            context, value.projectiles, [&](ReplaceModel& item) -> bool {
                YSM_LEGACY_TRY(ParseReplaceModel(context, item));
                return true;
            }));
    } else {
        OrderedMap<std::string, ReplaceModel> projectiles;
        YSM_LEGACY_TRY(ReadMap(
            context, projectiles,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                return true;
            },
            [&](ReplaceModel& item) -> bool {
                YSM_LEGACY_TRY(ParseReplaceModel(context, item));
                return true;
            }));
        for (auto& [key, item] : projectiles) {
            item.source_key = key;
            item.match.emplace_back(key);
            value.projectiles.emplace_back(std::move(item));
        }
        if (context.version >= 21) {
            OrderedMap<std::string, ReplaceModel> vehicles;
            YSM_LEGACY_TRY(ReadMap(
                context, vehicles,
                [&](std::string& key) -> bool {
                    YSM_LEGACY_TRY(ReadString(context, key));
                    return true;
                },
                [&](ReplaceModel& item) -> bool {
                    YSM_LEGACY_TRY(ParseReplaceModel(context, item));
                    return true;
                }));
            for (auto& [key, item] : vehicles) {
                item.source_key = key;
                item.match.emplace_back(key);
                value.vehicles.emplace_back(std::move(item));
            }
        }
    }
    YSM_LEGACY_TRY(
        ReadOptional(context, value.player, [&](PlayerModel& item) -> bool {
            YSM_LEGACY_TRY(ParsePlayerModel(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ParseModelInfo(context, value.info));
    YSM_LEGACY_TRY(
        ReadOptional(context, value.export_info, [&](ExportInfo& item) -> bool {
            YSM_LEGACY_TRY(ParseExportInfo(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ReadString(context, value.order_info));
    return true;
}

struct LegacyTexture {
    LegacyImage uv;
    OrderedMap<PbrType, LegacyImage> pbr;
};

struct LegacyTextureHash {
    std::string uv;
    OrderedMap<PbrType, std::string> pbr;
};

struct LegacyInfo {
    std::optional<ModelMetadata> metadata;
    ModelProperties properties;
};

bool ParseLegacyTexture(auto& context, LegacyTexture& value) {
    YSM_LEGACY_TRY(ParseImage(context, value.uv, true, ImageRole::kTexture));
    YSM_LEGACY_TRY(ReadMap(
        context, value.pbr,
        [&](PbrType& key) -> bool {
            YSM_LEGACY_TRY(ParsePbrType(context, key));
            return true;
        },
        [&](LegacyImage& image) -> bool {
            YSM_LEGACY_TRY(
                ParseImage(context, image, true, ImageRole::kTexture));
            return true;
        }));
    return true;
}

bool ParseLegacyTextureHash(auto& context, LegacyTextureHash& value) {
    YSM_LEGACY_TRY(ReadStringBytes(context, value.uv));
    YSM_LEGACY_TRY(ReadMap(
        context, value.pbr,
        [&](PbrType& key) -> bool {
            YSM_LEGACY_TRY(ParsePbrType(context, key));
            return true;
        },
        [&](std::string& hash) -> bool {
            YSM_LEGACY_TRY(ReadStringBytes(context, hash));
            return true;
        }));
    return true;
}

bool ParseLegacyInfo(auto& context, LegacyInfo& value) {
    YSM_LEGACY_TRY(
        ReadOptional(context, value.metadata, [&](ModelMetadata& item) -> bool {
            YSM_LEGACY_TRY(ParseMetadata(context, item));
            return true;
        }));
    YSM_LEGACY_TRY(ParseModelProperties(context, value.properties));
    return true;
}

std::string LegacyTextureName(std::uint32_t version, std::string name) {
    if (version != 1) {
        return name;
    }
    if (name == "arrow.png") {
        return "/ARROW\\";
    }
    constexpr std::string_view kSuffix = ".png";
    if (name.size() >= kSuffix.size() && name.ends_with(kSuffix)) {
        name.resize(name.size() - kSuffix.size());
    }
    return name;
}

template <typename Key, typename Value>
Value* Find(OrderedMap<Key, Value>& values, const Key& key) {
    const auto found = std::ranges::find_if(
        values, [&](const auto& entry) { return entry.first == key; });
    return found == values.end() ? nullptr : &found->second;
}

bool ParseLegacyFamily(auto& context, LegacyModel& value) {
    std::vector<std::uint32_t> features;
    YSM_LEGACY_TRY(
        ReadVector(context, features, [&](std::uint32_t& item) -> bool {
            YSM_LEGACY_TRY(ReadVarU32(context, item));
            if (item < 1 || item > 3) {
                YSM_LEGACY_INVALID(
                    "Historical feature enum has an unknown value");
            }
            return true;
        }));

    OrderedMap<std::uint32_t, std::optional<GeoModel>> geos;
    YSM_LEGACY_TRY(ReadMap(
        context, geos,
        [&](std::uint32_t& key) -> bool {
            YSM_LEGACY_TRY(ReadVarU32(context, key));
            if (key < 1 || key > 3) {
                YSM_LEGACY_INVALID(
                    "Historical model type has an unknown value");
            }
            return true;
        },
        [&](std::optional<GeoModel>& item) -> bool {
            YSM_LEGACY_TRY(
                ReadOptional(context, item, [&](GeoModel& geo) -> bool {
                    YSM_LEGACY_TRY(ParseGeoModel(context, geo));
                    return true;
                }));
            return true;
        }));

    OrderedMap<std::uint32_t, std::optional<AnimationFile>> animations;
    YSM_LEGACY_TRY(ReadMap(
        context, animations,
        [&](std::uint32_t& key) -> bool {
            YSM_LEGACY_TRY(ReadVarU32(context, key));
            if (key < 1 || key > 11) {
                YSM_LEGACY_INVALID(
                    "Historical animation type has an unknown value");
            }
            return true;
        },
        [&](std::optional<AnimationFile>& item) -> bool {
            YSM_LEGACY_TRY(ReadOptional(
                context, item, [&](AnimationFile& animation) -> bool {
                    YSM_LEGACY_TRY(ParseAnimationFile(context, animation));
                    return true;
                }));
            return true;
        }));

    std::vector<std::optional<AnimationControllerFile>> controllers;
    OrderedMap<std::string, std::string> controller_hashes;
    if (context.version >= 10) {
        YSM_LEGACY_TRY(ReadVector(
            context, controllers,
            [&](std::optional<AnimationControllerFile>& item) -> bool {
                YSM_LEGACY_TRY(ReadOptional(
                    context, item,
                    [&](AnimationControllerFile& controller) -> bool {
                        YSM_LEGACY_TRY(
                            ParseAnimationControllerFile(context, controller));
                        return true;
                    }));
                return true;
            }));
        YSM_LEGACY_TRY(ReadStringSourceHashMap(context, controller_hashes));
    }

    OrderedMap<std::string, LegacyTexture> textures;
    if (context.version >= 3) {
        YSM_LEGACY_TRY(ReadMap(
            context, textures,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                return true;
            },
            [&](LegacyTexture& item) -> bool {
                YSM_LEGACY_TRY(ParseLegacyTexture(context, item));
                return true;
            }));
    } else {
        OrderedMap<std::string, std::optional<LegacyImage>> old_textures;
        YSM_LEGACY_TRY(ReadMap(
            context, old_textures,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                return true;
            },
            [&](std::optional<LegacyImage>& item) -> bool {
                YSM_LEGACY_TRY(ReadOptional(
                    context, item, [&](LegacyImage& image) -> bool {
                        YSM_LEGACY_TRY(ParseImage(context, image, true,
                                                  ImageRole::kTexture));
                        return true;
                    }));
                return true;
            }));
        for (auto& [name, image] : old_textures) {
            if (!image) {
                YSM_LEGACY_INVALID("Historical texture is unexpectedly null");
            }
            LegacyTexture texture;
            texture.uv = std::move(*image);
            textures.emplace_back(std::move(name), std::move(texture));
        }
    }

    OrderedMap<std::string, LegacySound> sounds;
    OrderedMap<std::string, std::string> sound_hashes;
    if (context.version >= 11) {
        YSM_LEGACY_TRY(ReadLegacySoundMap(context, sounds));
        YSM_LEGACY_TRY(ReadStringSourceHashMap(context, sound_hashes));
    }

    OrderedMap<std::string, std::string> functions;
    OrderedMap<std::string, std::string> function_hashes;
    if (context.version >= 16) {
        YSM_LEGACY_TRY(ReadStringMap(context, functions));
        YSM_LEGACY_TRY(ReadStringSourceHashMap(context, function_hashes));
    }

    OrderedMap<std::string, LanguageFile> languages;
    OrderedMap<std::string, std::string> language_hashes;
    if (context.version >= 18) {
        YSM_LEGACY_TRY(ReadMap(
            context, languages,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                return true;
            },
            [&](LanguageFile& language) -> bool {
                YSM_LEGACY_TRY(ParseLanguageFile(context, language));
                return true;
            }));
        YSM_LEGACY_TRY(ReadStringSourceHashMap(context, language_hashes));
    }

    OrderedMap<std::string, LegacyImage> author_avatars;
    if (context.version >= 4) {
        YSM_LEGACY_TRY(ReadMap(
            context, author_avatars,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                return true;
            },
            [&](LegacyImage& image) -> bool {
                YSM_LEGACY_TRY(
                    ParseImage(context, image, true, ImageRole::kAvatar));
                return true;
            }));
    }

    OrderedMap<std::uint32_t, std::string> geo_hashes;
    YSM_LEGACY_TRY(ReadMap(
        context, geo_hashes,
        [&](std::uint32_t& key) -> bool {
            YSM_LEGACY_TRY(ReadVarU32(context, key));
            if (key < 1 || key > 3) {
                YSM_LEGACY_INVALID("Historical model hash type is invalid");
            }
            return true;
        },
        [&](std::string& hash) -> bool {
            YSM_LEGACY_TRY(ReadStringBytes(context, hash));
            return true;
        }));

    OrderedMap<std::uint32_t, std::string> animation_hashes;
    YSM_LEGACY_TRY(ReadMap(
        context, animation_hashes,
        [&](std::uint32_t& key) -> bool {
            YSM_LEGACY_TRY(ReadVarU32(context, key));
            if (key < 1 || key > 11) {
                YSM_LEGACY_INVALID("Historical animation hash type is invalid");
            }
            return true;
        },
        [&](std::string& hash) -> bool {
            YSM_LEGACY_TRY(ReadStringBytes(context, hash));
            return true;
        }));

    OrderedMap<std::string, LegacyTextureHash> texture_hashes;
    if (context.version >= 3) {
        YSM_LEGACY_TRY(ReadMap(
            context, texture_hashes,
            [&](std::string& key) -> bool {
                YSM_LEGACY_TRY(ReadString(context, key));
                return true;
            },
            [&](LegacyTextureHash& item) -> bool {
                YSM_LEGACY_TRY(ParseLegacyTextureHash(context, item));
                return true;
            }));
    } else {
        OrderedMap<std::string, std::string> old_hashes;
        YSM_LEGACY_TRY(ReadStringSourceHashMap(context, old_hashes));
        for (auto& [name, hash] : old_hashes) {
            LegacyTextureHash item;
            item.uv = std::move(hash);
            texture_hashes.emplace_back(std::move(name), std::move(item));
        }
    }

    YSM_LEGACY_TRY(ReadStringBytes(context, value.info.hash));
    std::optional<LegacyInfo> info;
    if (context.version >= 2) {
        YSM_LEGACY_TRY(
            ReadOptional(context, info, [&](LegacyInfo& item) -> bool {
                YSM_LEGACY_TRY(ParseLegacyInfo(context, item));
                return true;
            }));
    }
    if (context.version >= 6) {
        YSM_LEGACY_TRY(ReadOptional(
            context, value.export_info, [&](ExportInfo& item) -> bool {
                YSM_LEGACY_TRY(ParseExportInfo(context, item));
                return true;
            }));
    }

    value.player.emplace();
    const auto ensure_arrow = [&]() -> ReplaceModel& {
        if (value.projectiles.empty()) {
            ReplaceModel projectile;
            projectile.source_key = "arrow";
            projectile.match.emplace_back("arrow");
            value.projectiles.emplace_back(std::move(projectile));
        }
        return value.projectiles.front();
    };
    for (auto& [type, geo] : geos) {
        if (type == 3 && !geo) {
            static_cast<void>(ensure_arrow());
            continue;
        }
        if (!geo) {
            continue;
        }
        WithSourceHash<GeoModel> wrapped;
        if (const auto* hash = Find(geo_hashes, type)) {
            wrapped.source_hash = *hash;
        }
        wrapped.value = std::move(*geo);
        if (type == 3) {
            ensure_arrow().geo.emplace(std::move(wrapped));
        } else {
            value.player->geo_models.emplace_back(
                type == 1 ? PlayerModelType::kMain : PlayerModelType::kArm,
                std::move(wrapped));
        }
    }
    for (auto& [type, animation] : animations) {
        if (!animation) {
            continue;
        }
        WithSourceHash<AnimationFile> wrapped;
        if (const auto* hash = Find(animation_hashes, type)) {
            wrapped.source_hash = *hash;
        }
        wrapped.value = std::move(*animation);
        if (type == 5) {
            ensure_arrow().animation.emplace(std::move(wrapped));
        } else {
            if (!IsPlayerAnimationType(type)) {
                YSM_LEGACY_TARGET_REPRESENTATION(
                    "Historical animation type has no target identity");
            }
            value.player->animations.emplace_back(
                static_cast<PlayerAnimationType>(type), std::move(wrapped));
        }
    }
    if (controllers.size() != controller_hashes.size()) {
        YSM_LEGACY_TARGET_REPRESENTATION(
            "Historical controller names do not match controller values");
    }
    for (std::size_t index = 0; index < controllers.size(); ++index) {
        if (!controllers[index]) {
            continue;
        }
        WithSourceHash<AnimationControllerFile> wrapped;
        wrapped.source_hash = controller_hashes[index].second;
        wrapped.value = std::move(*controllers[index]);
        value.player->controllers.emplace_back(controller_hashes[index].first,
                                               std::move(wrapped));
    }

    for (auto& [raw_name, texture] : textures) {
        auto name = LegacyTextureName(context.version, std::move(raw_name));
        auto* hashes = Find(texture_hashes, name);
        if (hashes == nullptr && context.version == 1) {
            for (auto& [raw_hash_name, item] : texture_hashes) {
                if (LegacyTextureName(context.version, raw_hash_name) == name) {
                    hashes = &item;
                    break;
                }
            }
        }
        TextureSet converted;
        if (hashes) {
            converted.uv.source_hash = hashes->uv;
        }
        converted.uv.value = std::move(texture.uv);
        for (auto& [type, image] : texture.pbr) {
            WithSourceHash<LegacyImage> wrapped;
            if (hashes) {
                if (const auto* hash = Find(hashes->pbr, type)) {
                    wrapped.source_hash = *hash;
                }
            }
            wrapped.value = std::move(image);
            converted.pbr.emplace_back(type, std::move(wrapped));
        }
        if (name == "/ARROW\\") {
            ensure_arrow().texture.emplace(std::move(converted));
        } else {
            value.player->textures.emplace_back(std::move(name),
                                                std::move(converted));
        }
    }

    for (auto& [name, sound] : sounds) {
        WithSourceHash<LegacySound> wrapped;
        if (const auto* hash = Find(sound_hashes, name)) {
            wrapped.source_hash = *hash;
        }
        wrapped.value = std::move(sound);
        value.common.sounds.emplace_back(std::move(name), std::move(wrapped));
    }
    for (auto& [name, function] : functions) {
        WithSourceHash<std::string> wrapped;
        if (const auto* hash = Find(function_hashes, name)) {
            wrapped.source_hash = *hash;
        }
        wrapped.value = std::move(function);
        value.common.user_functions.emplace_back(std::move(name),
                                                 std::move(wrapped));
    }
    for (auto& [name, language] : languages) {
        WithSourceHash<LanguageFile> wrapped;
        if (const auto* hash = Find(language_hashes, name)) {
            wrapped.source_hash = *hash;
        }
        wrapped.value = std::move(language);
        value.common.languages.emplace_back(std::move(name),
                                            std::move(wrapped));
    }

    value.info.author_avatars = std::move(author_avatars);
    if (info) {
        value.info.metadata = std::move(info->metadata);
        value.info.properties = std::move(info->properties);
    } else {
        const auto* main =
            Find(value.player->geo_models, PlayerModelType::kMain);
        const auto* properties =
            main == nullptr ? nullptr : &main->value.properties;
        if (properties != nullptr) {
            value.info.properties.height_scale =
                static_cast<float>(properties->height_scale);
            value.info.properties.width_scale =
                static_cast<float>(properties->width_scale);
        }
        if (!value.player->textures.empty()) {
            value.info.properties.default_texture =
                value.player->textures.front().first;
        }
        if (properties != nullptr && properties->extra_info) {
            ModelMetadata metadata;
            metadata.name = properties->extra_info->name;
            metadata.tips = properties->extra_info->tips;
            metadata.license.type = properties->extra_info->license;
            for (const auto& author : properties->extra_info->authors) {
                metadata.authors.push_back(ModelAuthor{.name = author});
            }
            value.info.metadata.emplace(std::move(metadata));
            value.info.properties.free = properties->extra_info->free;
            for (std::size_t index = 0;
                 index < properties->extra_info->extra_animation_names.size();
                 ++index) {
                value.info.properties.extra_animation.emplace_back(
                    "extra" + std::to_string(index),
                    properties->extra_info->extra_animation_names[index]);
            }
        } else {
            for (int index = 0; index < 8; ++index) {
                value.info.properties.extra_animation.emplace_back(
                    "extra" + std::to_string(index), "");
            }
        }
    }
    return true;
}

template <typename Key, typename Value>
const Value* Find(const OrderedMap<Key, Value>& values, const Key& key) {
    const auto found = std::ranges::find_if(
        values, [&](const auto& entry) { return entry.first == key; });
    return found == values.end() ? nullptr : &found->second;
}

bool DecodeModelId(auto& context, std::string_view hash,
                   BufferFixed<32>& result) {
    if (hash.size() != 32) {
        YSM_LEGACY_INVALID(
            "Historical model hash is not 32 hexadecimal characters");
    }
    const auto nibble = [](char value) -> std::optional<Byte> {
        if (value >= '0' && value <= '9') {
            return static_cast<Byte>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<Byte>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<Byte>(value - 'A' + 10);
        }
        return std::nullopt;
    };
    result = {};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto high = nibble(hash[index * 2]);
        const auto low = nibble(hash[index * 2 + 1]);
        if (!high || !low) {
            YSM_LEGACY_INVALID(
                "Historical model hash contains a non-hex character");
        }
        result[index] = static_cast<Byte>((*high << 4U) | *low);
    }
    return true;
}

bool ParseRoot(auto& context, LegacyModel& value) {
    YSM_LEGACY_TRY(ReadFixedU32(context, context.version));
    if (context.version < 1 || context.version > 32) {
        YSM_LEGACY_UNSUPPORTED("Historical model version is unsupported");
    }
    value.version = context.version;
    if (context.version < 19) {
        YSM_LEGACY_TRY(ParseLegacyFamily(context, value));
    } else {
        YSM_LEGACY_TRY(ParseCurrentFamily(context, value));
    }
    YSM_LEGACY_TRY(DecodeModelId(context, value.info.hash, value.model_id));
    value.sound_field_count = context.sound_field_count;
    value.omitted_sound_count = context.omitted_sound_count;
    return true;
}
}  // namespace

class BufferReader final {
   public:
    explicit BufferReader(BufferViewR bytes) : remaining_(bytes) {}

    [[nodiscard]] bool ReadExact(BufferView destination) {
        if (destination.size() > remaining_.size()) {
            return Fail(
                absl::DataLossError("Historical model plaintext is truncated"));
        }
        Copy(Slice(remaining_, 0, destination.size()), destination);
        remaining_ = remaining_.subspan(destination.size());
        offset_ += destination.size();
        return true;
    }

    [[nodiscard]] bool Finish() {
        return remaining_.empty()
                   ? true
                   : Fail(absl::DataLossError(
                         "Historical model has trailing plaintext bytes"));
    }

    [[nodiscard]] const absl::Status& status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

   private:
    bool Fail(absl::Status status) {
        status_ = std::move(status);
        return false;
    }

    BufferViewR remaining_;
    std::uint64_t offset_{};
    absl::Status status_;
};

template <typename ReaderType>
absl::StatusOr<LegacyModel> DecodeImpl(ReaderType& reader) {
    Context context{reader};
    LegacyModel model;
    if (!ParseRoot(context, model)) {
        return std::move(context.status);
    }
    if (!reader.Finish()) {
        return reader.status();
    }
    return model;
}

absl::StatusOr<LegacyModel> Decode(Reader& reader) {
    return DecodeImpl(reader);
}

absl::StatusOr<LegacyModel> Decode(BufferViewR bytes) {
    BufferReader reader(bytes);
    return DecodeImpl(reader);
}
}  // namespace ysm::legacy::v3::container
