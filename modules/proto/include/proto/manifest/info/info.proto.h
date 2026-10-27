#pragma once
#include <optional>
#include <ylt/struct_pb.hpp>
#include "proto/manifest/info/properties.proto.h"
#include "proto/manifest/info/metadata.proto.h"
#include "proto/manifest/info/settings.proto.h"
#include "proto/manifest/info/export_info.proto.h"
#include "proto/manifest/info/language_file.proto.h"

namespace ysm::proto::manifest::info {

enum class PreviewSource {
    kUnspecified = 0,
    kRaw = 1,
    kGenerated = 2,
};

struct Info : public iguana::base_impl<Info>  {
	Info() = default;
	Info(std::vector<::ysm::proto::manifest::info::LanguageFile>a, ::ysm::proto::manifest::info::Properties b, ::ysm::proto::manifest::info::Settings c, std::optional<::ysm::proto::manifest::info::Metadata> d, std::optional<::ysm::proto::manifest::info::ExportInfo> e, std::optional<PreviewSource> f, std::optional<PreviewSource> g) : language_files(std::move(a)), properties(b), settings(c), metadata(std::move(d)), export_(std::move(e)), thumbnail_source(f), icon_source(g) {}
	std::vector<::ysm::proto::manifest::info::LanguageFile>language_files;
	::ysm::proto::manifest::info::Properties properties;
	::ysm::proto::manifest::info::Settings settings;
	std::optional<::ysm::proto::manifest::info::Metadata> metadata;
	std::optional<::ysm::proto::manifest::info::ExportInfo> export_;
	std::optional<PreviewSource> thumbnail_source;
	std::optional<PreviewSource> icon_source;
};
YLT_REFL(Info, language_files, properties, settings, metadata, export_, thumbnail_source, icon_source);

}
