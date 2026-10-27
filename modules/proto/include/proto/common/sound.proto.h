#pragma once
#include <optional>
#include <ylt/struct_pb.hpp>

namespace ysm::proto::common {

struct Sound {
	std::string name;
	std::string encoding;
	uint32_t channels;
	uint32_t sample_rate;
	uint64_t samples;
	std::string first_page;
	std::vector<uint32_t>chunk_ids;
};
YLT_REFL(Sound, name, encoding, channels, sample_rate, samples, first_page, chunk_ids);

}
