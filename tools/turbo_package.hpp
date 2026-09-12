#pragma once
#include <string>

namespace obd::convert {
// Synchronous converter-only deterministic upstream TurboOCI metadata archive.
// An empty gzip_index_path omits gzip.meta (uncompressed tar target).
void write_turbo_package(const std::string& metadata_path,
                         const std::string& gzip_index_path,
                         const std::string& output_path);
}
