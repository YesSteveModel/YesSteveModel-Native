#pragma once
#include <ylt/struct_pb.hpp>
#include "proto/manifest/asset/texture.proto.h"
#include "proto/manifest/info/model_settings.proto.h"
#include "proto/manifest/info/model_stats.proto.h"

namespace ysm::proto::manifest::asset {


enum class RenderTargetKind {
    kUnspecified = 0,
    kPlayer = 1,
    kProjectile = 3,
    kVehicle = 4,
};

struct TexturesEntry : public iguana::base_impl<TexturesEntry>  {
	TexturesEntry() = default;
	TexturesEntry(std::string a, ::ysm::proto::manifest::asset::PBRTextureSet b) : key(std::move(a)), value(b) {}
	std::string key;
	::ysm::proto::manifest::asset::PBRTextureSet value;
};
YLT_REFL(TexturesEntry, key, value);

struct RenderTarget : public iguana::base_impl<RenderTarget>  {
	RenderTarget() = default;
	RenderTarget(std::string a, RenderTargetKind b, std::vector<std::string>c, uint32_t d, std::vector<TexturesEntry>e, ::ysm::proto::manifest::info::ModelSettings f, ::ysm::proto::manifest::info::ModelStats g) : target_id(std::move(a)), kind(b), match(std::move(c)), blob_id(d), textures(std::move(e)), settings(f), stats(g) {}
	std::string target_id;
	RenderTargetKind kind;
	std::vector<std::string>match;
	uint32_t blob_id;
	std::vector<TexturesEntry>textures;
	::ysm::proto::manifest::info::ModelSettings settings;
	::ysm::proto::manifest::info::ModelStats stats;
};
YLT_REFL(RenderTarget, target_id, kind, match, blob_id, textures, settings, stats);

}
