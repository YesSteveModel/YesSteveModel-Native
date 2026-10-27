#pragma once
#include <ylt/struct_pb.hpp>
#include "proto/asset/model/data/geo_model.proto.h"
#include "proto/asset/model/data/animation.proto.h"
#include "proto/asset/model/data/animation_controller.proto.h"

namespace ysm::proto::asset::model {


struct GeoModelsEntry : public iguana::base_impl<GeoModelsEntry>  {
	GeoModelsEntry() = default;
	GeoModelsEntry(std::string a, ::ysm::proto::asset::model::data::GeoModel b) : key(std::move(a)), value(std::move(b)) {}
	std::string key;
	::ysm::proto::asset::model::data::GeoModel value;
};
YLT_REFL(GeoModelsEntry, key, value);

struct AnimationFilesEntry : public iguana::base_impl<AnimationFilesEntry>  {
	AnimationFilesEntry() = default;
	AnimationFilesEntry(std::string a, ::ysm::proto::asset::model::data::AnimationFile b) : key(std::move(a)), value(b) {}
	std::string key;
	::ysm::proto::asset::model::data::AnimationFile value;
};
YLT_REFL(AnimationFilesEntry, key, value);

struct AnimationControllersEntry : public iguana::base_impl<AnimationControllersEntry>  {
	AnimationControllersEntry() = default;
	AnimationControllersEntry(std::string a, ::ysm::proto::asset::model::data::AnimationControllerFile b) : key(std::move(a)), value(b) {}
	std::string key;
	::ysm::proto::asset::model::data::AnimationControllerFile value;
};
YLT_REFL(AnimationControllersEntry, key, value);

struct ModelData : public iguana::base_impl<ModelData>  {
	ModelData() = default;
	ModelData(std::vector<GeoModelsEntry>a, std::vector<AnimationFilesEntry>b, std::vector<AnimationControllersEntry>c) : geo_models(std::move(a)), animation_files(std::move(b)), animation_controllers(std::move(c)) {}
	std::vector<GeoModelsEntry>geo_models;
	std::vector<AnimationFilesEntry>animation_files;
	std::vector<AnimationControllersEntry>animation_controllers;
};
YLT_REFL(ModelData, geo_models, animation_files, animation_controllers);

}
