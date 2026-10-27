#pragma once
#include <ylt/struct_pb.hpp>

namespace ysm::proto::manifest::info {

struct ExportInfo : public iguana::base_impl<ExportInfo>  {
	ExportInfo() = default;
	ExportInfo(uint64_t a, std::string b) : timestamp(a), extra(std::move(b)) {}
	uint64_t timestamp;
	std::string extra;
};
YLT_REFL(ExportInfo, timestamp, extra);

}
