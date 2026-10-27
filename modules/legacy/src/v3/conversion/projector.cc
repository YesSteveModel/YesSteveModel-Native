#include "projector.h"

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <ylt/struct_pb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <buffer.h>
#include <err.h>

#include <v3/container/image.h>
#include "proto/asset/model/ModelData.proto.h"
#include "proto/asset/strings/StringData.proto.h"
#include "proto/manifest/manifest.proto.h"

namespace ysm::legacy::v3::conversion {
namespace {
namespace proto = ysm::proto;
namespace common_proto = proto::common;
namespace model_proto = proto::asset::model;
namespace data_proto = proto::asset::model::data;
namespace strings_proto = proto::asset::strings;
namespace string_data_proto = proto::asset::strings::data;
namespace manifest_proto = proto::manifest;
namespace asset_proto = proto::manifest::asset;
namespace info_proto = proto::manifest::info;
using container::ImageRole;

using PositionIndexMap =
    absl::flat_hash_map<std::array<float, 3>, std::uint32_t>;
using UvIndexMap = absl::flat_hash_map<std::array<float, 2>, std::uint32_t>;

bool UnsignedUtf8Less(std::string_view left, std::string_view right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(),
        [](char lhs, char rhs) {
            return static_cast<unsigned char>(lhs) <
                   static_cast<unsigned char>(rhs);
        });
}

template <typename Value>
absl::StatusOr<std::vector<const std::pair<std::string, Value>*>>
SortedStringEntries(const container::OrderedMap<std::string, Value>& values,
                    std::string_view field) {
    std::vector<const std::pair<std::string, Value>*> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(&value);
    }
    std::ranges::sort(result, [](const auto* left, const auto* right) {
        return UnsignedUtf8Less(left->first, right->first);
    });
    for (std::size_t index = 1; index < result.size(); ++index) {
        if (result[index - 1]->first == result[index]->first) {
            return absl::FailedPreconditionError("Duplicate target key in " +
                                                 std::string(field));
        }
    }
    return result;
}

template <typename Value>
absl::StatusOr<std::vector<std::pair<std::string, Value>*>> SortedStringEntries(
    container::OrderedMap<std::string, Value>& values, std::string_view field) {
    std::vector<std::pair<std::string, Value>*> result;
    result.reserve(values.size());
    for (auto& value : values) {
        result.push_back(&value);
    }
    std::ranges::sort(result, [](const auto* left, const auto* right) {
        return UnsignedUtf8Less(left->first, right->first);
    });
    for (std::size_t index = 1; index < result.size(); ++index) {
        if (result[index - 1]->first == result[index]->first) {
            return absl::FailedPreconditionError("Duplicate target key in " +
                                                 std::string(field));
        }
    }
    return result;
}

absl::StatusOr<std::vector<common_proto::StringPair>> ProjectStringMap(
    const container::OrderedMap<std::string, std::string>& source,
    std::string_view field) {
    std::vector<common_proto::StringPair> result;
    YSM_DECLARE_OR_RETURN(entries, SortedStringEntries(source, field));
    for (const auto* entry : entries) {
        result.emplace_back(entry->first, entry->second);
    }
    return result;
}

absl::StatusOr<std::vector<info_proto::EntriesEntry>> ProjectLanguageMap(
    const container::OrderedMap<std::string, std::string>& source) {
    std::vector<info_proto::EntriesEntry> result;
    YSM_DECLARE_OR_RETURN(entries,
                          SortedStringEntries(source, "language entries"));
    result.reserve(entries.size());
    for (const auto* entry : entries) {
        result.emplace_back(entry->first, entry->second);
    }
    return result;
}

common_proto::Program SourceProgram(std::string source) {
    common_proto::Program result{};
    result.payload.emplace<1>(std::move(source));
    return result;
}

std::string JoinStatements(const std::vector<std::string>& sources) {
    std::string result;
    for (const auto& source : sources) {
        result.append(source);
        const auto last = source.find_last_not_of(" \t\r\n");
        if (last == std::string::npos || source[last] != ';') {
            result.push_back(';');
        }
        result.push_back('\n');
    }
    return result;
}

bool IsBlank(std::string_view value) {
    return value.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos;
}

absl::StatusOr<float> ProjectFloat(double value, std::string_view field) {
    const auto projected = static_cast<float>(value);
    if (std::isfinite(value)) {
        if (!std::isfinite(projected)) {
            return absl::FailedPreconditionError(
                "Finite value overflows target float in " + std::string(field));
        }
        return projected;
    }
    if ((std::isnan(value) && std::isnan(projected)) ||
        (std::isinf(value) && std::isinf(projected) &&
         std::signbit(value) == std::signbit(projected))) {
        return projected;
    }
    return absl::FailedPreconditionError(
        "Non-finite value changes category in " + std::string(field));
}

template <typename Target, typename Source>
absl::StatusOr<Target> ProjectEnum(Source value, std::string_view field) {
    using SourceValue = std::underlying_type_t<Source>;
    const auto raw =
        static_cast<std::uint64_t>(static_cast<SourceValue>(value));
    if (raw > static_cast<std::uint64_t>(
                  (std::numeric_limits<std::int32_t>::max)())) {
        return absl::FailedPreconditionError(
            "Enum value exceeds target int32 in " + std::string(field));
    }
    return static_cast<Target>(static_cast<std::int32_t>(raw));
}

template <typename Message>
absl::Status SerializeProto(const Message& message, std::string_view field,
                            std::string& output) {
    try {
        output.clear();
        struct_pb::to_pb(message, output);
        Message decoded{};
        decoded.from_pb(output);
        return absl::OkStatus();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        return absl::FailedPreconditionError(
            "Target protobuf round trip failed for " + std::string(field) +
            ": " + error.what());
    }
}

absl::StatusOr<BufferManaged> CopyBytes(std::string_view bytes) {
    if (bytes.size() > BufferManaged::kMaxSize) {
        return absl::FailedPreconditionError(
            "Target payload exceeds native buffer range");
    }
    return BufferManaged(StrBuf(bytes));
}

struct ModelStats {
    std::uint32_t bones{};
    std::uint32_t cubes{};
    std::uint32_t faces{};
};

absl::StatusOr<data_proto::ExpressionValue> ProjectMolang(
    const container::MolangValue& source) {
    data_proto::ExpressionValue result{};
    switch (source.type) {
        case container::MolangValueType::kNone:
            return absl::FailedPreconditionError(
                "Legacy Molang value has no active variant");
        case container::MolangValueType::kDouble: {
            float value;
            YSM_ASSIGN_OR_RETURN(
                value, ProjectFloat(source.double_value, "Molang number"));
            result.value = value;
        } break;
        case container::MolangValueType::kString:
            result.value = SourceProgram(source.string_value);
            break;
    }
    return result;
}

absl::StatusOr<std::vector<data_proto::ExpressionValue>> ProjectMolangVec3(
    const container::MolangVec3& source) {
    std::vector<data_proto::ExpressionValue> result;
    result.reserve(3);
    for (const auto* value : {&source.x, &source.y, &source.z}) {
        data_proto::ExpressionValue projected;
        YSM_ASSIGN_OR_RETURN(projected, ProjectMolang(*value));
        result.emplace_back(std::move(projected));
    }
    return result;
}

absl::StatusOr<data_proto::BoneKeyFrame> ProjectBoneKeyFrame(
    const container::BoneKeyFrame& source) {
    data_proto::BoneKeyFrame result{};
    YSM_ASSIGN_OR_RETURN(result.start_tick,
                         ProjectFloat(source.start_tick, "bone keyframe tick"));
    YSM_ASSIGN_OR_RETURN(
        result.easing_type,
        ProjectEnum<data_proto::EasingType>(source.easing, "easing type"));
    YSM_ASSIGN_OR_RETURN(result.pre, ProjectMolangVec3(source.pre));
    if (source.post) {
        std::vector<data_proto::ExpressionValue> post;
        YSM_ASSIGN_OR_RETURN(post, ProjectMolangVec3(*source.post));
        result.post = std::move(post);
    }
    return result;
}

absl::StatusOr<data_proto::BoneAnimation> ProjectBoneAnimation(
    const container::BoneAnimation& source) {
    data_proto::BoneAnimation result{};
    result.bone_name = source.bone_name;
    const auto append = [](const auto& values, auto& output) -> absl::Status {
        output.reserve(values.size());
        for (const auto& value : values) {
            data_proto::BoneKeyFrame projected;
            YSM_ASSIGN_OR_RETURN(projected, ProjectBoneKeyFrame(value));
            output.emplace_back(std::move(projected));
        }
        return absl::OkStatus();
    };
    YSM_RETURN_IF_ERROR(append(source.rotations, result.rotation));
    YSM_RETURN_IF_ERROR(append(source.positions, result.position));
    YSM_RETURN_IF_ERROR(append(source.scales, result.scale));
    return result;
}

absl::StatusOr<data_proto::Animation> ProjectAnimation(
    const container::Animation& source) {
    data_proto::Animation result{};
    result.name = source.name;
    YSM_ASSIGN_OR_RETURN(result.length,
                         ProjectFloat(source.length, "animation length"));
    YSM_ASSIGN_OR_RETURN(result.loop, ProjectEnum<data_proto::LoopType>(
                                          source.loop, "loop type"));
    if (source.blend_weight) {
        data_proto::ExpressionValue blend_weight;
        YSM_ASSIGN_OR_RETURN(blend_weight, ProjectMolang(*source.blend_weight));
        result.blend_weight = std::move(blend_weight);
    }
    result.bone_animations.reserve(source.bones.size());
    for (const auto& bone : source.bones) {
        data_proto::BoneAnimation projected;
        YSM_ASSIGN_OR_RETURN(projected, ProjectBoneAnimation(bone));
        result.bone_animations.emplace_back(std::move(projected));
    }
    result.instruction_keyframes.reserve(source.instructions.size());
    for (const auto& frame : source.instructions) {
        data_proto::InstructionKeyFrame projected{};
        projected.programs = SourceProgram(JoinStatements(frame.data));
        YSM_ASSIGN_OR_RETURN(
            projected.start_tick,
            ProjectFloat(frame.start_tick, "instruction keyframe tick"));
        result.instruction_keyframes.emplace_back(std::move(projected));
    }
    result.sound_keyframes.reserve(source.sounds.size());
    for (const auto& frame : source.sounds) {
        float start_tick{};
        YSM_ASSIGN_OR_RETURN(
            start_tick, ProjectFloat(frame.start_tick, "sound keyframe tick"));
        result.sound_keyframes.emplace_back(frame.data, start_tick);
    }
    return result;
}

absl::StatusOr<data_proto::AnimationFile> ProjectAnimationFile(
    const container::AnimationFile& source) {
    data_proto::AnimationFile result{};
    result.animations.reserve(source.animations.size());
    for (const auto& animation : source.animations) {
        data_proto::Animation projected;
        YSM_ASSIGN_OR_RETURN(projected, ProjectAnimation(animation));
        result.animations.emplace_back(std::move(projected));
    }
    return result;
}

absl::StatusOr<data_proto::AnimationControllerFile> ProjectControllerFile(
    const container::AnimationControllerFile& source) {
    data_proto::AnimationControllerFile result{};
    YSM_DECLARE_OR_RETURN(
        controller_entries,
        SortedStringEntries(source.controllers, "animation controllers"));
    for (const auto* controller_entry : controller_entries) {
        const auto& source_controller = controller_entry->second;
        data_proto::AnimationController controller{};
        controller.name = controller_entry->first;
        if (!source_controller.initial_state.empty()) {
            controller.default_state = source_controller.initial_state;
        }
        YSM_DECLARE_OR_RETURN(
            state_entries,
            SortedStringEntries(source_controller.states, "controller states"));
        for (const auto* state_entry : state_entries) {
            const auto& source_state = state_entry->second;
            data_proto::State state{};
            state.name = state_entry->first;
            state.animations.reserve(source_state.animations.size());
            for (const auto& animation : source_state.animations) {
                state.animations.emplace_back(
                    animation.name, SourceProgram(animation.condition.empty()
                                                      ? "true"
                                                      : animation.condition));
            }
            state.transitions.reserve(source_state.transitions.size());
            for (const auto& transition : source_state.transitions) {
                state.transitions.emplace_back(
                    transition.destination,
                    SourceProgram(transition.condition));
            }
            if (!source_state.on_entry.empty()) {
                state.on_entry =
                    SourceProgram(JoinStatements(source_state.on_entry));
            }
            if (!source_state.on_exit.empty()) {
                state.on_exit =
                    SourceProgram(JoinStatements(source_state.on_exit));
            }
            data_proto::BlendTransition blend_transition{};
            bool has_blend_transition = false;
            if (source_state.blend_transition.linear_length) {
                float linear_length{};
                YSM_ASSIGN_OR_RETURN(
                    linear_length,
                    ProjectFloat(*source_state.blend_transition.linear_length,
                                 "controller blend length"));
                blend_transition.linear_length = linear_length;
                has_blend_transition = true;
            }
            blend_transition.points.reserve(
                source_state.blend_transition.points.size());
            for (const auto& [key, value] :
                 source_state.blend_transition.points) {
                float projected_key{};
                float projected_value{};
                YSM_ASSIGN_OR_RETURN(
                    projected_key,
                    ProjectFloat(key, "controller blend point key"));
                YSM_ASSIGN_OR_RETURN(
                    projected_value,
                    ProjectFloat(value, "controller blend point value"));
                blend_transition.points.emplace_back(projected_key,
                                                     projected_value);
                has_blend_transition = true;
            }
            if (has_blend_transition) {
                state.blend_transition = std::move(blend_transition);
            }
            state.sound_effects = source_state.sound_effects;
            controller.states.emplace_back(std::move(state));
        }
        result.controllers.emplace_back(std::move(controller));
    }
    return result;
}

absl::StatusOr<model_proto::GeoModelsEntry> ProjectGeoEntry(
    std::string key, const container::GeoModel& source, ModelStats& stats,
    PositionIndexMap& position_indices, UvIndexMap& uv_indices) {
    data_proto::GeoModel geometry{};
    geometry.bones.reserve(source.bones.size());
    stats.bones = static_cast<std::uint32_t>(source.bones.size());
    for (const auto& source_bone : source.bones) {
        data_proto::Bone bone{};
        bone.name = source_bone.name;
        if (!source_bone.parent.empty()) {
            bone.parent = source_bone.parent;
        }
        bone.pivot = {source_bone.pivot.x, source_bone.pivot.y,
                      source_bone.pivot.z};
        bone.rotate = {source_bone.rotation.x, source_bone.rotation.y,
                       source_bone.rotation.z};
        const auto cube_count =
            static_cast<std::uint32_t>(source_bone.cubes.size());
        bone.cubeCount = cube_count;
        stats.cubes += cube_count;
        geometry.bones.emplace_back(std::move(bone));
        for (const auto& source_cube : source_bone.cubes) {
            position_indices.clear();
            uv_indices.clear();
            data_proto::CubeLegacy cube{};
            cube.faceCount =
                static_cast<std::uint32_t>(source_cube.quads.size());
            stats.faces += cube.faceCount;
            for (const auto& quad : source_cube.quads) {
                for (std::uint32_t index = 0; index < quad.vertices.size();
                     ++index) {
                    const auto& vertex = quad.vertices[index];
                    const std::array position{vertex.position.x,
                                              vertex.position.y,
                                              vertex.position.z};
                    const auto [position_entry, position_inserted] =
                        position_indices.try_emplace(
                            position,
                            static_cast<std::uint32_t>(position_indices.size()));
                    if (position_inserted) {
                        cube.pos.insert(cube.pos.end(), position.begin(),
                                        position.end());
                    }
                    cube.pos_indices.push_back(position_entry->second);

                    const std::array uv{vertex.texture_u, vertex.texture_v};
                    const auto [uv_entry, uv_inserted] = uv_indices.try_emplace(
                        uv, static_cast<std::uint32_t>(uv_indices.size()));
                    if (uv_inserted) {
                        cube.uv.insert(cube.uv.end(), uv.begin(), uv.end());
                    }
                    cube.uv_indices.push_back(uv_entry->second);
                }
                cube.normal.insert(
                    cube.normal.end(),
                    {quad.normal.x, quad.normal.y, quad.normal.z});
            }
            geometry.cubes.cubes_legacy.emplace_back(std::move(cube));
        }
    }
    YSM_ASSIGN_OR_RETURN(
        geometry.properties.texture_height,
        ProjectFloat(source.properties.texture_height, "texture height"));
    YSM_ASSIGN_OR_RETURN(
        geometry.properties.texture_width,
        ProjectFloat(source.properties.texture_width, "texture width"));
    model_proto::GeoModelsEntry result{};
    result.key = std::move(key);
    result.value = std::move(geometry);
    return result;
}

absl::StatusOr<std::string> PlayerGeoName(container::PlayerModelType type) {
    switch (type) {
        case container::PlayerModelType::kMain:
            return "main";
        case container::PlayerModelType::kArm:
            return "arm";
    }
    return absl::FailedPreconditionError(
        "Player geometry type has no target key");
}

absl::StatusOr<std::string> PlayerAnimationName(
    container::PlayerAnimationType type) {
    switch (type) {
        case container::PlayerAnimationType::kMain:
            return "main";
        case container::PlayerAnimationType::kArm:
            return "arm";
        case container::PlayerAnimationType::kExtra:
            return "extra";
        case container::PlayerAnimationType::kTac:
            return "tac";
        case container::PlayerAnimationType::kCarryOn:
            return "carryon";
        case container::PlayerAnimationType::kParCool:
            return "parcool";
        case container::PlayerAnimationType::kSwem:
            return "swem";
        case container::PlayerAnimationType::kSlashBlade:
            return "slashblade";
        case container::PlayerAnimationType::kTlm:
            return "tlm";
        case container::PlayerAnimationType::kFirstPersonArm:
            return "fp_arm";
        case container::PlayerAnimationType::kImmersiveMelodies:
            return "immersive_melodies";
        case container::PlayerAnimationType::kIronsSpellBooks:
            return "irons_spell_books";
    }
    return absl::FailedPreconditionError(
        "Player animation type has no target key");
}

info_proto::ModelSettings ProjectModelSettings(
    const container::ModelProperties& source) {
    info_proto::ModelSettings result{};
    result.height_scale = source.height_scale;
    result.width_scale = source.width_scale;
    result.render_layers_first = source.render_layers_first;
    result.force_culling = source.all_cutout;
    result.gui_no_lighting = source.gui_no_lighting;
    result.merge_multiline_expr = source.merge_multiline_expr;
    return result;
}

absl::StatusOr<info_proto::ConfigForms> ProjectConfigForm(
    const container::ConfigForm& source) {
    info_proto::ConfigForms result{};
    result.type = source.type;
    result.title = source.title;
    result.description = source.description;
    const auto inert = IsBlank(source.value);
    result.read_program = SourceProgram(inert ? "0" : source.value);
    result.write_program =
        SourceProgram(inert ? "return;" : source.value + "=t.value");
    YSM_ASSIGN_OR_RETURN(result.step, ProjectFloat(source.step, "config step"));
    YSM_ASSIGN_OR_RETURN(result.min,
                         ProjectFloat(source.minimum, "config minimum"));
    YSM_ASSIGN_OR_RETURN(result.max,
                         ProjectFloat(source.maximum, "config maximum"));
    YSM_DECLARE_OR_RETURN(labels,
                          SortedStringEntries(source.labels, "config labels"));
    result.labels.reserve(labels.size());
    for (const auto* label : labels) {
        result.labels.emplace_back(label->first, SourceProgram(label->second));
    }
    return result;
}

struct EncodedImage {
    PayloadEncoding encoding{};
    std::string token;
    std::uint32_t width{};
    std::uint32_t height{};
    BufferManaged bytes;
};

absl::StatusOr<std::pair<PayloadEncoding, std::string_view>> ImageEncoding(
    container::ImageEncoding encoding) {
    switch (encoding) {
        case container::ImageEncoding::kPng:
            return std::pair{PayloadEncoding::kPng, std::string_view{"PNG"}};
        case container::ImageEncoding::kJpeg:
            return std::pair{PayloadEncoding::kJpeg, std::string_view{"JPEG"}};
        case container::ImageEncoding::kWebp:
            return std::pair{PayloadEncoding::kWebp, std::string_view{"WEBP"}};
        case container::ImageEncoding::kAvif:
            return std::pair{PayloadEncoding::kAvif, std::string_view{"AVIF"}};
        case container::ImageEncoding::kZtx:
            return std::pair{PayloadEncoding::kZtx, std::string_view{"ZTX"}};
        case container::ImageEncoding::kRgba:
            break;
    }
    return absl::FailedPreconditionError(
        "Shared image codec produced an invalid format");
}

absl::StatusOr<EncodedImage> TakePreparedImage(container::LegacyImage& source,
                                               container::ImageRole role) {
    YSM_RETURN_IF_ERROR(container::ValidatePreparedImage(source, role));
    std::pair<PayloadEncoding, std::string_view> metadata;
    YSM_ASSIGN_OR_RETURN(metadata, ImageEncoding(source.encoding));
    const auto [encoding, token] = metadata;
    return EncodedImage{encoding, std::string(token), source.width,
                        source.height, std::move(source.bytes)};
}

class ImageCollector final {
   public:
    explicit ImageCollector(std::uint32_t next_blob_id)
        : next_blob_id_(next_blob_id) {}

    absl::StatusOr<common_proto::Image> AddBlob(container::LegacyImage& source,
                                                ImageRole role) {
        if (const auto cached = blob_cache_.find(&source);
            cached != blob_cache_.end()) {
            if (cached->second.role != role || source.role != role) {
                return absl::FailedPreconditionError(
                    "Prepared image was reused with a different role");
            }
            const auto& previous = blob_payloads_[cached->second.payload_index];
            std::pair<PayloadEncoding, std::string_view> metadata;
            YSM_ASSIGN_OR_RETURN(metadata, ImageEncoding(source.encoding));
            const auto [encoding, token] = metadata;
            const auto blob_id = next_blob_id_++;
            common_proto::Image image{};
            image.blob_id = blob_id;
            image.format = token;
            image.width = source.width;
            image.height = source.height;
            image.frame_count = 1;
            blob_payloads_.push_back(
                Payload{PayloadKind::kBlobImage, encoding, blob_id, "",
                        ImageMetadata{source.width, source.height, 1},
                        BufferManaged(previous.bytes)});
            return image;
        }

        EncodedImage encoded;
        YSM_ASSIGN_OR_RETURN(encoded, TakePreparedImage(source, role));
        const auto blob_id = next_blob_id_++;
        common_proto::Image image{};
        image.blob_id = blob_id;
        image.format = encoded.token;
        image.width = encoded.width;
        image.height = encoded.height;
        image.frame_count = 1;
        blob_payloads_.push_back(
            Payload{PayloadKind::kBlobImage, encoded.encoding, blob_id, "",
                    ImageMetadata{encoded.width, encoded.height, 1},
                    std::move(encoded.bytes)});
        blob_cache_.emplace(&source,
                            CachedBlob{blob_payloads_.size() - 1, source.role});
        return image;
    }

    absl::Status AddNamed(container::LegacyImage& source, ImageRole role,
                          std::string name) {
        EncodedImage encoded;
        YSM_ASSIGN_OR_RETURN(encoded, TakePreparedImage(source, role));
        named_payloads_.push_back(Payload{
            PayloadKind::kNamedImage, encoded.encoding, 0, std::move(name),
            ImageMetadata{encoded.width, encoded.height, 1},
            std::move(encoded.bytes)});
        return absl::OkStatus();
    }

    std::vector<Payload> TakeBlobPayloads() {
        return std::move(blob_payloads_);
    }

    std::vector<Payload> TakeNamedPayloads() {
        return std::move(named_payloads_);
    }

   private:
    struct CachedBlob {
        std::size_t payload_index;
        ImageRole role;
    };

    std::uint32_t next_blob_id_;
    std::vector<Payload> blob_payloads_;
    std::vector<Payload> named_payloads_;
    std::unordered_map<const container::LegacyImage*, CachedBlob> blob_cache_;
};

absl::StatusOr<asset_proto::PBRTextureSet> ProjectTextureSet(
    container::TextureSet& source, ImageCollector& images) {
    asset_proto::PBRTextureSet result{};
    YSM_ASSIGN_OR_RETURN(result.uv,
                         images.AddBlob(source.uv.value, ImageRole::kTexture));
    container::LegacyImage* normal = nullptr;
    container::LegacyImage* specular = nullptr;
    for (auto& [type, image] : source.pbr) {
        switch (type) {
            case container::PbrType::kNormal:
                if (normal != nullptr) {
                    return absl::FailedPreconditionError(
                        "Duplicate normal texture");
                }
                normal = &image.value;
                break;
            case container::PbrType::kSpecular:
                if (specular != nullptr) {
                    return absl::FailedPreconditionError(
                        "Duplicate specular texture");
                }
                specular = &image.value;
                break;
            default:
                return absl::FailedPreconditionError(
                    "PBR type has no target field");
        }
    }
    if (normal != nullptr) {
        common_proto::Image projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             images.AddBlob(*normal, ImageRole::kTexture));
        result.normal = std::move(projected);
    }
    if (specular != nullptr) {
        common_proto::Image projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             images.AddBlob(*specular, ImageRole::kTexture));
        result.specular = std::move(projected);
    }
    return result;
}

absl::StatusOr<info_proto::Settings> ProjectInfoSettings(
    const container::ModelProperties& source) {
    info_proto::Settings result{};
    YSM_ASSIGN_OR_RETURN(
        result.extra_animation,
        ProjectStringMap(source.extra_animation, "extra animations"));
    if (result.extra_animation.empty()) {
        for (int index = 0; index < 8; ++index) {
            result.extra_animation.emplace_back("extra" + std::to_string(index),
                                                "");
        }
    }
    result.extra_animation_buttons.reserve(
        source.extra_animation_buttons.size());
    for (const auto& source_button : source.extra_animation_buttons) {
        info_proto::ExtraAnimationButton button{};
        button.id = source_button.id;
        button.name = source_button.name;
        button.sound = source_button.sound;
        button.config_forms.reserve(source_button.forms.size());
        for (const auto& form : source_button.forms) {
            info_proto::ConfigForms projected;
            YSM_ASSIGN_OR_RETURN(projected, ProjectConfigForm(form));
            button.config_forms.emplace_back(std::move(projected));
        }
        result.extra_animation_buttons.emplace_back(std::move(button));
    }
    result.extra_animation_classify.reserve(
        source.extra_animation_classify.size());
    for (const auto& source_classify : source.extra_animation_classify) {
        info_proto::ExtraAnimationClassify classify{};
        classify.id = source_classify.id;
        YSM_ASSIGN_OR_RETURN(classify.extra_animation,
                             ProjectStringMap(source_classify.animations,
                                              "classified extra animations"));
        result.extra_animation_classify.emplace_back(std::move(classify));
    }
    if (!source.default_texture.empty()) {
        result.default_texture = source.default_texture;
    }
    if (!source.preview_animation.empty()) {
        result.preview_animation = source.preview_animation;
    }
    result.disable_preview_rotation = source.disable_preview_rotation;
    return result;
}

struct TargetProjection {
    asset_proto::RenderTarget manifest{};
    model_proto::ModelData model_data{};
};

absl::StatusOr<TargetProjection> ProjectPlayer(
    container::PlayerModel& source,
    const container::ModelProperties& properties, std::uint32_t blob_id,
    ImageCollector& images, PositionIndexMap& position_indices,
    UvIndexMap& uv_indices) {
    if (source.textures.empty()) {
        return absl::FailedPreconditionError("Player target has no texture");
    }
    TargetProjection result{};
    result.manifest.target_id = "player";
    result.manifest.kind =
        asset_proto::RenderTargetKind::kPlayer;
    result.manifest.blob_id = blob_id;
    result.manifest.settings = ProjectModelSettings(properties);

    std::vector<std::pair<std::string, const container::GeoModel*>> geos;
    for (const auto& [type, geo] : source.geo_models) {
        std::string name;
        YSM_ASSIGN_OR_RETURN(name, PlayerGeoName(type));
        geos.emplace_back(std::move(name), &geo.value);
    }
    std::ranges::sort(geos, [](const auto& left, const auto& right) {
        return UnsignedUtf8Less(left.first, right.first);
    });
    for (std::size_t index = 1; index < geos.size(); ++index) {
        if (geos[index - 1].first == geos[index].first) {
            return absl::FailedPreconditionError(
                "Duplicate player geometry target key");
        }
    }
    ModelStats player_stats;
    for (const auto& [name, geo] : geos) {
        ModelStats stats;
        model_proto::GeoModelsEntry projected;
        YSM_ASSIGN_OR_RETURN(
            projected,
            ProjectGeoEntry(name, *geo, stats, position_indices, uv_indices));
        result.model_data.geo_models.emplace_back(std::move(projected));
        if (name == "main") {
            player_stats = stats;
        }
    }
    result.manifest.stats = {player_stats.bones, player_stats.cubes,
                             player_stats.faces};

    std::vector<std::pair<std::string, const container::AnimationFile*>>
        animations;
    for (const auto& [type, animation] : source.animations) {
        std::string name;
        YSM_ASSIGN_OR_RETURN(name, PlayerAnimationName(type));
        animations.emplace_back(std::move(name), &animation.value);
    }
    std::ranges::sort(animations, [](const auto& left, const auto& right) {
        return UnsignedUtf8Less(left.first, right.first);
    });
    for (std::size_t index = 1; index < animations.size(); ++index) {
        if (animations[index - 1].first == animations[index].first) {
            return absl::FailedPreconditionError(
                "Duplicate player animation target key");
        }
    }
    for (const auto& [name, animation] : animations) {
        data_proto::AnimationFile projected;
        YSM_ASSIGN_OR_RETURN(projected, ProjectAnimationFile(*animation));
        result.model_data.animation_files.emplace_back(name,
                                                       std::move(projected));
    }
    YSM_DECLARE_OR_RETURN(
        controllers,
        SortedStringEntries(source.controllers, "player controllers"));
    for (const auto* controller : controllers) {
        data_proto::AnimationControllerFile projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             ProjectControllerFile(controller->second.value));
        result.model_data.animation_controllers.emplace_back(
            controller->first, std::move(projected));
    }
    YSM_DECLARE_OR_RETURN(
        textures, SortedStringEntries(source.textures, "player textures"));
    for (auto* texture : textures) {
        asset_proto::PBRTextureSet projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             ProjectTextureSet(texture->second, images));
        result.manifest.textures.emplace_back(texture->first,
                                              std::move(projected));
    }
    return result;
}

absl::StatusOr<TargetProjection> ProjectReplacement(
    container::ReplaceModel& source, std::string target_id,
    asset_proto::RenderTargetKind kind,
    const container::ModelProperties& properties, std::uint32_t blob_id,
    ImageCollector& images, PositionIndexMap& position_indices,
    UvIndexMap& uv_indices) {
    TargetProjection result{};
    result.manifest.target_id = std::move(target_id);
    result.manifest.kind = kind;
    result.manifest.match = source.match;
    result.manifest.blob_id = blob_id;
    result.manifest.settings = ProjectModelSettings(properties);
    ModelStats stats;
    if (source.geo) {
        model_proto::GeoModelsEntry projected;
        YSM_ASSIGN_OR_RETURN(
            projected, ProjectGeoEntry("main", source.geo->value, stats,
                                       position_indices, uv_indices));
        result.model_data.geo_models.emplace_back(std::move(projected));
    }
    if (source.animation) {
        data_proto::AnimationFile projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             ProjectAnimationFile(source.animation->value));
        result.model_data.animation_files.emplace_back("main",
                                                       std::move(projected));
    }
    if (source.controller) {
        data_proto::AnimationControllerFile projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             ProjectControllerFile(source.controller->value));
        result.model_data.animation_controllers.emplace_back(
            "main", std::move(projected));
    }
    if (source.texture) {
        asset_proto::PBRTextureSet projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             ProjectTextureSet(*source.texture, images));
        result.manifest.textures.emplace_back("default", std::move(projected));
    }
    result.manifest.stats = {stats.bones, stats.cubes, stats.faces};
    return result;
}

absl::StatusOr<std::vector<container::ReplaceModel*>> OrderedReplacements(
    std::vector<container::ReplaceModel>& source, std::uint32_t version,
    std::string_view field) {
    std::vector<container::ReplaceModel*> result;
    result.reserve(source.size());
    for (auto& value : source) {
        result.push_back(&value);
    }
    if (version <= 26) {
        std::ranges::sort(result, [](const auto* left, const auto* right) {
            return UnsignedUtf8Less(left->source_key, right->source_key);
        });
        for (std::size_t index = 1; index < result.size(); ++index) {
            if (result[index - 1]->source_key == result[index]->source_key) {
                return absl::FailedPreconditionError(
                    "Duplicate target identity in " + std::string(field));
            }
        }
    }
    return result;
}

absl::StatusOr<info_proto::Info> ProjectInfo(container::LegacyModel& model,
                                             ImageCollector& images) {
    info_proto::Info result{};
    YSM_DECLARE_OR_RETURN(
        languages, SortedStringEntries(model.common.languages, "languages"));
    for (const auto* language : languages) {
        info_proto::LanguageFile projected{};
        projected.locale = language->first;
        YSM_ASSIGN_OR_RETURN(
            projected.entries,
            ProjectLanguageMap(language->second.value.entries));
        result.language_files.emplace_back(std::move(projected));
    }
    info_proto::Settings settings{};
    YSM_ASSIGN_OR_RETURN(settings, ProjectInfoSettings(model.info.properties));

    container::LegacyImage* gui_foreground = nullptr;
    container::LegacyImage* gui_background = nullptr;
    container::LegacyImage* thumb_button = nullptr;
    container::LegacyImage* thumb_icon = nullptr;
    YSM_DECLARE_OR_RETURN(
        gui_images, SortedStringEntries(model.info.gui_images, "GUI images"));
    for (auto* entry : gui_images) {
        if (entry->first == "gui_foreground") {
            gui_foreground = &entry->second;
        } else if (entry->first == "gui_background") {
            gui_background = &entry->second;
        } else if (entry->first == "thumb-button") {
            thumb_button = &entry->second;
        } else if (entry->first == "thumb-icon") {
            thumb_icon = &entry->second;
        } else {
            return absl::FailedPreconditionError(
                "GUI image has no target role: " + entry->first);
        }
    }
    if (gui_foreground != nullptr) {
        common_proto::Image projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             images.AddBlob(*gui_foreground, ImageRole::kGui));
        settings.gui_foreground = std::move(projected);
    }
    if (gui_background != nullptr) {
        common_proto::Image projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             images.AddBlob(*gui_background, ImageRole::kGui));
        settings.gui_background = std::move(projected);
    }
    result.settings = std::move(settings);

    std::unordered_set<std::string> used_avatars;
    if (model.info.metadata) {
        const auto& source = *model.info.metadata;
        info_proto::Metadata metadata{};
        metadata.name = source.name;
        if (!source.tips.empty()) {
            metadata.tips = source.tips;
        }
        metadata.license.type = source.license.type;
        if (!source.license.description.empty()) {
            metadata.license.desc = source.license.description;
        }
        YSM_ASSIGN_OR_RETURN(metadata.links,
                             ProjectStringMap(source.links, "metadata links"));
        metadata.authors.reserve(source.authors.size());
        for (const auto& source_author : source.authors) {
            info_proto::Author author{};
            author.name = source_author.name;
            author.role = source_author.role;
            YSM_ASSIGN_OR_RETURN(
                author.contacts,
                ProjectStringMap(source_author.contact, "author contacts"));
            if (!source_author.comment.empty()) {
                author.comment = source_author.comment;
            }
            const auto avatar = std::ranges::find_if(
                model.info.author_avatars, [&](const auto& entry) {
                    return entry.first == source_author.name;
                });
            if (avatar != model.info.author_avatars.end()) {
                common_proto::Image projected;
                YSM_ASSIGN_OR_RETURN(
                    projected,
                    images.AddBlob(avatar->second, ImageRole::kAvatar));
                author.avatar = std::move(projected);
                used_avatars.emplace(avatar->first);
            }
            metadata.authors.emplace_back(std::move(author));
        }
        result.metadata = std::move(metadata);
    }
    if (used_avatars.size() != model.info.author_avatars.size()) {
        return absl::FailedPreconditionError(
            "Historical author avatar has no target author occurrence");
    }

    result.properties.model_id.assign(
        reinterpret_cast<const char*>(model.model_id.data()),
        model.model_id.size());
    result.properties.free = model.info.properties.free;
    result.properties.origin_ver = std::to_string(model.info.origin_version);
    if (model.export_info) {
        info_proto::ExportInfo export_info{};
        export_info.timestamp = model.export_info->timestamp;
        export_info.extra = model.export_info->extra;
        result.export_ = std::move(export_info);
    }
    if (thumb_button != nullptr) {
        YSM_RETURN_IF_ERROR(images.AddNamed(
            *thumb_button, ImageRole::kThumbnail, "thumb-button"));
        result.thumbnail_source = info_proto::PreviewSource::kRaw;
    }
    if (thumb_icon != nullptr) {
        YSM_RETURN_IF_ERROR(
            images.AddNamed(*thumb_icon, ImageRole::kIcon, "thumb-icon"));
        result.icon_source = info_proto::PreviewSource::kRaw;
    }
    return result;
}

absl::StatusOr<strings_proto::StringData> ProjectStringData(
    const container::CommonAssets& common) {
    strings_proto::StringData result{};
    YSM_DECLARE_OR_RETURN(functions, SortedStringEntries(common.user_functions,
                                                         "user functions"));
    for (const auto* function : functions) {
        result.user_functions.emplace_back(
            function->first, SourceProgram(function->second.value));
    }
    return result;
}

absl::StatusOr<Payload> DirectPayload(PayloadKind kind,
                                      std::uint32_t logical_id,
                                      std::string name,
                                      std::string_view bytes) {
    BufferManaged payload;
    YSM_ASSIGN_OR_RETURN(payload, CopyBytes(bytes));
    return Payload{kind,         PayloadEncoding::kDirect,
                   logical_id,   std::move(name),
                   std::nullopt, std::move(payload)};
}

absl::StatusOr<std::size_t> ProjectedPayloadCount(
    const container::LegacyModel& model) {
    constexpr std::size_t kMaxPayloadCount = 32'766;
    std::size_t count = 2;
    auto add = [&](std::size_t additional) {
        if (additional > kMaxPayloadCount - count) {
            return false;
        }
        count += additional;
        return true;
    };
    const auto add_texture = [&](const container::TextureSet& texture) {
        return add(1) && add(texture.pbr.size());
    };

    if (!add(model.player ? 1 : 0) || !add(model.projectiles.size()) ||
        !add(model.vehicles.size())) {
        return absl::ResourceExhaustedError("Too many legacy payload records");
    }
    if (model.player) {
        for (const auto& [_, texture] : model.player->textures) {
            if (!add_texture(texture)) {
                return absl::ResourceExhaustedError(
                    "Too many legacy payload records");
            }
        }
    }
    for (const auto& replacement : model.projectiles) {
        if (replacement.texture && !add_texture(*replacement.texture)) {
            return absl::ResourceExhaustedError(
                "Too many legacy payload records");
        }
    }
    for (const auto& replacement : model.vehicles) {
        if (replacement.texture && !add_texture(*replacement.texture)) {
            return absl::ResourceExhaustedError(
                "Too many legacy payload records");
        }
    }
    for (const auto& [name, _] : model.info.gui_images) {
        if ((name == "gui_foreground" || name == "gui_background" ||
             name == "thumb-button" || name == "thumb-icon") &&
            !add(1)) {
            return absl::ResourceExhaustedError(
                "Too many legacy payload records");
        }
    }
    if (model.info.metadata) {
        for (const auto& author : model.info.metadata->authors) {
            if (std::ranges::find_if(model.info.author_avatars,
                                     [&](const auto& entry) {
                                         return entry.first == author.name;
                                     }) != model.info.author_avatars.end() &&
                !add(1)) {
                return absl::ResourceExhaustedError(
                    "Too many legacy payload records");
            }
        }
    }
    if (!add(model.common.sounds.size())) {
        return absl::ResourceExhaustedError("Too many legacy payload records");
    }
    return count;
}

absl::StatusOr<ImportResult> BuildProjection(container::LegacyModel& model,
                                             std::uint64_t source_size) {
    std::size_t payload_count{};
    YSM_ASSIGN_OR_RETURN(payload_count, ProjectedPayloadCount(model));
    std::vector<container::ReplaceModel*> projectiles;
    YSM_ASSIGN_OR_RETURN(projectiles,
                         OrderedReplacements(model.projectiles, model.version,
                                             "projectile targets"));
    std::vector<container::ReplaceModel*> vehicles;
    YSM_ASSIGN_OR_RETURN(
        vehicles,
        OrderedReplacements(model.vehicles, model.version, "vehicle targets"));
    const auto target_count =
        (model.player ? 1U : 0U) + projectiles.size() + vehicles.size();
    ImageCollector images(static_cast<std::uint32_t>(2 + target_count));
    PositionIndexMap position_indices;
    position_indices.reserve(8);
    UvIndexMap uv_indices;
    uv_indices.reserve(24);
    std::vector<TargetProjection> targets;
    targets.reserve(target_count);
    std::uint32_t next_model_blob = 2;
    if (model.player) {
        TargetProjection projected;
        YSM_ASSIGN_OR_RETURN(projected,
                             ProjectPlayer(*model.player, model.info.properties,
                                           next_model_blob++, images,
                                           position_indices, uv_indices));
        targets.emplace_back(std::move(projected));
    }
    std::uint32_t ordinal = 1;
    for (auto* projectile : projectiles) {
        TargetProjection projected;
        YSM_ASSIGN_OR_RETURN(
            projected,
            ProjectReplacement(
                *projectile, "projectile-" + std::to_string(ordinal++),
                asset_proto::RenderTargetKind::kProjectile,
                model.info.properties, next_model_blob++, images,
                position_indices, uv_indices));
        targets.emplace_back(std::move(projected));
    }
    ordinal = 1;
    for (auto* vehicle : vehicles) {
        TargetProjection projected;
        YSM_ASSIGN_OR_RETURN(
            projected,
            ProjectReplacement(
                *vehicle, "vehicle-" + std::to_string(ordinal++),
                asset_proto::RenderTargetKind::kVehicle,
                model.info.properties, next_model_blob++, images,
                position_indices, uv_indices));
        targets.emplace_back(std::move(projected));
    }

    manifest_proto::Manifest manifest{};
    manifest.render_targets.reserve(targets.size());
    for (const auto& target : targets) {
        manifest.render_targets.push_back(target.manifest);
    }
    manifest.common_assets.strings_blob_id = 1;
    YSM_ASSIGN_OR_RETURN(manifest.info, ProjectInfo(model, images));
    YSM_DECLARE_OR_RETURN(
        sounds, SortedStringEntries(model.common.sounds, "sound streams"));

    std::vector<Payload> payloads;
    payloads.reserve(payload_count);
    std::string serialization_scratch;
    YSM_RETURN_IF_ERROR(
        SerializeProto(manifest, "Manifest", serialization_scratch));
    Payload manifest_payload;
    YSM_ASSIGN_OR_RETURN(
        manifest_payload,
        DirectPayload(PayloadKind::kManifest, 0, "", serialization_scratch));
    payloads.emplace_back(std::move(manifest_payload));

    strings_proto::StringData string_data;
    YSM_ASSIGN_OR_RETURN(string_data, ProjectStringData(model.common));
    YSM_RETURN_IF_ERROR(
        SerializeProto(string_data, "StringData", serialization_scratch));
    Payload string_data_payload;
    YSM_ASSIGN_OR_RETURN(
        string_data_payload,
        DirectPayload(PayloadKind::kStringData, 1, "", serialization_scratch));
    payloads.emplace_back(std::move(string_data_payload));
    for (std::size_t index = 0; index < targets.size(); ++index) {
        YSM_RETURN_IF_ERROR(SerializeProto(targets[index].model_data,
                                           "ModelData", serialization_scratch));
        Payload model_data_payload;
        YSM_ASSIGN_OR_RETURN(
            model_data_payload,
            DirectPayload(
                PayloadKind::kModelData, static_cast<std::uint32_t>(index + 2),
                targets[index].manifest.target_id, serialization_scratch));
        payloads.emplace_back(std::move(model_data_payload));
    }
    auto blob_images = images.TakeBlobPayloads();
    for (auto& payload : blob_images) {
        payloads.emplace_back(std::move(payload));
    }
    auto named_images = images.TakeNamedPayloads();
    std::ranges::sort(
        named_images, [](const Payload& left, const Payload& right) {
            return left.name == "thumb-button" && right.name == "thumb-icon";
        });
    for (auto& payload : named_images) {
        payloads.emplace_back(std::move(payload));
    }
    std::uint32_t sound_id = 1;
    for (auto* sound : sounds) {
        auto& source = sound->second.value;
        const auto encoding =
            source.encoding == container::SoundEncoding::kVorbis
                ? PayloadEncoding::kOggVorbis
                : PayloadEncoding::kOggOpus;
        payloads.push_back(Payload{PayloadKind::kSoundStream, encoding,
                                   sound_id++, sound->first, std::nullopt,
                                   std::move(source.bytes),
                                   SoundMetadata{source.channels,
                                                 source.sample_rate,
                                                 source.samples}});
    }
    return ImportResult{{model.version, source_size, model.model_id},
                        std::move(payloads)};
}
}  // namespace

absl::StatusOr<ImportResult> Convert(container::LegacyModel model,
                                     std::uint64_t source_size) {
    return BuildProjection(model, source_size);
}
}  // namespace ysm::legacy::v3::conversion
