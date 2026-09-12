#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace obd::convert {
/// Converts bottom-up original OCI tar/gzip layers into a differential warp
/// stack. Destination must not exist; publishes a validated private sibling
/// stage atomically without replacing an existing path. Original blobs stay
/// external. Result paths refer to the published destination.
nlohmann::json convert_turbo_layers(const std::vector<std::string>& inputs,
                                    const std::string& output_directory,
                                    uint64_t requested_size = 0,
                                    bool keep_raw = false);
}
