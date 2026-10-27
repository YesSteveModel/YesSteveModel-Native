#include "result_owner.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <buffer.h>
#include <err.h>

namespace ysm::legacy::java {
namespace {
constexpr std::size_t kHeaderSize = 60;
constexpr std::size_t kPayloadPrefixSize = 40;
constexpr std::size_t kMaxDescriptorSize = 16 * 1024 * 1024;
constexpr std::size_t kMaxPayloadCount = 32'766;
constexpr std::uint64_t kMaxSourceSize = 69'206'016;
constexpr std::uint32_t kMaxImageDimension = 4096;

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

bool UnsignedUtf8Less(std::string_view lhs, std::string_view rhs) noexcept {
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](char left, char right) {
            return static_cast<unsigned char>(left) <
                   static_cast<unsigned char>(right);
        });
}

bool IsImageEncoding(PayloadEncoding encoding) noexcept {
    return (encoding >= PayloadEncoding::kPng &&
            encoding <= PayloadEncoding::kAvif) ||
           encoding == PayloadEncoding::kZtx;
}

bool IsSoundEncoding(PayloadEncoding encoding) noexcept {
    return encoding == PayloadEncoding::kOggVorbis ||
           encoding == PayloadEncoding::kOggOpus;
}

absl::Status ValidatePayload(const Payload& payload) {
    YSM_ASSERT(IsValidUtf8(payload.name),
               absl::InvalidArgumentError("Payload name is not UTF-8"));
    switch (payload.kind) {
        case PayloadKind::kManifest:
            YSM_ASSERT(payload.encoding == PayloadEncoding::kDirect &&
                           payload.logical_id == 0 && payload.name.empty() &&
                           !payload.image && !payload.sound,
                       absl::InvalidArgumentError(
                           "Invalid Manifest payload metadata"));
            return absl::OkStatus();
        case PayloadKind::kStringData:
            YSM_ASSERT(payload.encoding == PayloadEncoding::kDirect &&
                           payload.logical_id != 0 && payload.name.empty() &&
                           !payload.image && !payload.sound,
                       absl::InvalidArgumentError(
                           "Invalid StringData payload metadata"));
            return absl::OkStatus();
        case PayloadKind::kModelData:
            YSM_ASSERT(payload.encoding == PayloadEncoding::kDirect &&
                           payload.logical_id != 0 && !payload.name.empty() &&
                           !payload.image && !payload.sound,
                       absl::InvalidArgumentError(
                           "Invalid ModelData payload metadata"));
            return absl::OkStatus();
        case PayloadKind::kBlobImage:
        case PayloadKind::kNamedImage:
            YSM_ASSERT(
                IsImageEncoding(payload.encoding) && payload.image &&
                    payload.image->width != 0 && payload.image->height != 0 &&
                    payload.image->width <= kMaxImageDimension &&
                    payload.image->height <= kMaxImageDimension &&
                    payload.image->frame_count == 1 && !payload.sound,
                absl::InvalidArgumentError("Invalid image payload metadata"));
            if (payload.kind == PayloadKind::kBlobImage) {
                YSM_ASSERT(
                    payload.logical_id != 0 && payload.name.empty(),
                    absl::InvalidArgumentError("Invalid blob image identity"));
            } else {
                YSM_ASSERT(
                    payload.logical_id == 0 &&
                        (payload.name == "thumb-button" ||
                         payload.name == "thumb-icon"),
                    absl::InvalidArgumentError("Invalid named image identity"));
            }
            return absl::OkStatus();
        case PayloadKind::kSoundStream:
            YSM_ASSERT(
                IsSoundEncoding(payload.encoding) && payload.logical_id != 0 &&
                    !payload.name.empty() && !payload.image && payload.sound &&
                    (payload.sound->channels == 1 ||
                     payload.sound->channels == 2) &&
                    payload.sound->sample_rate != 0 &&
                    payload.sound->frames != 0,
                absl::InvalidArgumentError("Invalid sound stream metadata"));
            return absl::OkStatus();
    }
    return absl::InvalidArgumentError("Unknown payload kind");
}

absl::Status ValidateCanonicalOrder(const std::vector<Payload>& payloads) {
    YSM_ASSERT(payloads.size() >= 2 &&
                   payloads[0].kind == PayloadKind::kManifest &&
                   payloads[1].kind == PayloadKind::kStringData &&
                   payloads[1].logical_id == 1,
               absl::InvalidArgumentError(
                   "Payloads do not begin with Manifest and StringData"));
    int phase = 0;
    std::uint32_t next_blob_id = 1;
    std::uint32_t next_sound_id = 1;
    std::unordered_set<std::string> model_names;
    std::string_view previous_sound_name;
    bool saw_sound = false;
    bool saw_thumb_button = false;
    bool saw_thumb_icon = false;
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const auto& payload = payloads[index];
        YSM_RETURN_IF_ERROR(ValidatePayload(payload));
        switch (payload.kind) {
            case PayloadKind::kManifest:
                YSM_ASSERT(index == 0, absl::InvalidArgumentError(
                                           "Manifest payload is not first"));
                break;
            case PayloadKind::kStringData:
                YSM_ASSERT(index == 1 && payload.logical_id == next_blob_id++,
                           absl::InvalidArgumentError(
                               "StringData payload is not second"));
                break;
            case PayloadKind::kModelData:
                YSM_ASSERT(index >= 2 && phase == 0 &&
                               payload.logical_id == next_blob_id++ &&
                               model_names.emplace(payload.name).second,
                           absl::InvalidArgumentError(
                               "Noncanonical ModelData record"));
                break;
            case PayloadKind::kBlobImage:
                phase = std::max(phase, 1);
                YSM_ASSERT(phase == 1 && payload.logical_id == next_blob_id++,
                           absl::InvalidArgumentError(
                               "Noncanonical blob image record"));
                break;
            case PayloadKind::kNamedImage:
                phase = std::max(phase, 2);
                YSM_ASSERT(
                    phase == 2 &&
                        (payload.name == "thumb-button"
                             ? !std::exchange(saw_thumb_button, true)
                             : !std::exchange(saw_thumb_icon, true)) &&
                        !(payload.name == "thumb-button" && saw_thumb_icon),
                    absl::InvalidArgumentError(
                        "Noncanonical named image record"));
                break;
            case PayloadKind::kSoundStream:
                phase = 3;
                YSM_ASSERT(
                    payload.logical_id == next_sound_id++ &&
                        (!saw_sound ||
                         UnsignedUtf8Less(previous_sound_name, payload.name)),
                    absl::InvalidArgumentError(
                        "Sound streams are not in canonical order"));
                previous_sound_name = payload.name;
                saw_sound = true;
                break;
        }
        YSM_ASSERT(
            phase != 3 || payload.kind == PayloadKind::kSoundStream,
            absl::InvalidArgumentError("Payload appears after sound streams"));
    }
    return absl::OkStatus();
}

template <typename Integer>
void StoreLittleEndian(BufferView output, std::size_t& offset, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    auto bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output[offset++] = static_cast<Byte>(bits & 0xFFU);
        bits >>= 8U;
    }
}
}  // namespace

absl::StatusOr<BufferManaged> EncodeDescriptor(const ImportResult& result) {
    YSM_ASSERT(result.metadata.inner_version >= 1 &&
                   result.metadata.inner_version <= 32,
               absl::InvalidArgumentError("Invalid inner version"));
    YSM_ASSERT(result.metadata.source_size <= kMaxSourceSize,
               absl::ResourceExhaustedError("Source size exceeds limit"));
    YSM_ASSERT(result.payloads.size() <= kMaxPayloadCount,
               absl::ResourceExhaustedError("Payload count exceeds limit"));
    YSM_RETURN_IF_ERROR(ValidateCanonicalOrder(result.payloads));

    std::size_t descriptor_size = kHeaderSize;
    for (const auto& payload : result.payloads) {
        YSM_ASSERT(
            payload.name.size() <= kMaxDescriptorSize &&
                descriptor_size <= kMaxDescriptorSize - kPayloadPrefixSize &&
                descriptor_size + kPayloadPrefixSize <=
                    kMaxDescriptorSize - payload.name.size(),
            absl::ResourceExhaustedError("Descriptor size exceeds limit"));
        descriptor_size += kPayloadPrefixSize + payload.name.size();
    }

    BufferManaged output(descriptor_size);
    std::size_t offset = 0;
    constexpr BufferFixed<8> kMagic{'Y', 'S', 'M', 'L', 'E', 'G', 'I', 0};
    Copy(kMagic, Slice(output, offset, kMagic.size()));
    offset += kMagic.size();
    StoreLittleEndian(output, offset, result.metadata.inner_version);
    StoreLittleEndian(output, offset,
                      static_cast<std::uint32_t>(result.payloads.size()));
    StoreLittleEndian<std::uint32_t>(output, offset, 0);
    StoreLittleEndian(output, offset, result.metadata.source_size);
    Copy(result.metadata.model_id,
         Slice(output, offset, result.metadata.model_id.size()));
    offset += result.metadata.model_id.size();

    for (const auto& payload : result.payloads) {
        StoreLittleEndian(output, offset,
                          static_cast<std::uint16_t>(payload.kind));
        StoreLittleEndian(output, offset,
                          static_cast<std::uint16_t>(payload.encoding));
        StoreLittleEndian<std::uint32_t>(output, offset, 0);
        StoreLittleEndian(output, offset, payload.logical_id);
        StoreLittleEndian(output, offset,
                          static_cast<std::uint32_t>(payload.name.size()));
        StoreLittleEndian(output, offset,
                          payload.image ? payload.image->width
                                        : payload.sound
                                              ? payload.sound->channels
                                              : 0);
        StoreLittleEndian(output, offset,
                          payload.image ? payload.image->height
                                        : payload.sound
                                              ? payload.sound->sample_rate
                                              : 0);
        StoreLittleEndian<std::uint64_t>(
            output, offset,
            payload.image ? payload.image->frame_count
                          : payload.sound ? payload.sound->frames : 0);
        StoreLittleEndian(output, offset,
                          static_cast<std::uint64_t>(payload.bytes.size()));
        Copy(StrBuf(payload.name), Slice(output, offset, payload.name.size()));
        offset += payload.name.size();
    }
    YSM_ASSERT(offset == output.size(),
               absl::InternalError("Legacy descriptor size drifted"));
    return output;
}

absl::StatusOr<std::unique_ptr<ResultOwner>> ResultOwner::Create(
    ImportResult result) {
    BufferManaged descriptor;
    YSM_ASSIGN_OR_RETURN(descriptor, EncodeDescriptor(result));
    return std::unique_ptr<ResultOwner>(
        new ResultOwner(std::move(result), std::move(descriptor)));
}
}  // namespace ysm::legacy::java
