#pragma once
#include <string>
#include <cstdint>

namespace obd::convert {
inline constexpr uint64_t kDefaultTurboMetadataBudget = 1024ULL * 1024 * 1024;
// Synchronous converter-only deterministic upstream TurboOCI metadata archive.
// An empty gzip_index_path omits gzip.meta (uncompressed tar target).
void write_turbo_package(const std::string& metadata_path,
                         const std::string& gzip_index_path,
                         const std::string& output_path);
struct ImportedTurboPackage {
    std::string metadata_path;
    std::string gzip_index_path;
};
// Requires a nonexistent destination directory. Validates the complete archive
// in a private sibling directory before atomically publishing the destination.
// Rejects declared sizes before extraction against a configurable cumulative
// budget for metadata plus gzip index. Unique members bound total output.
// Supports only regular ext4.fs.meta, .turbo.ociv1 and optional gzip.meta members.
ImportedTurboPackage import_turbo_package(const std::string& package_path,
                                          const std::string& output_directory,
                                          uint64_t metadata_budget = kDefaultTurboMetadataBudget);
}
