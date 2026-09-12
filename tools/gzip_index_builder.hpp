#pragma once
#include <cstdint>
#include <string>

namespace obd::convert {
// Synchronous, converter-only utility. Publishes an index atomically after
// validating the complete, single-member gzip stream and trailer.
void build_gzip_index(const std::string& gzip_path, const std::string& index_path,
                      uint32_t span = 1048576);
}
