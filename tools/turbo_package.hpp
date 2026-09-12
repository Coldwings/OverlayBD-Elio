#pragma once
#include <string>

namespace obd::convert {
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
// Supports only regular ext4.fs.meta, .turbo.ociv1 and optional gzip.meta members.
ImportedTurboPackage import_turbo_package(const std::string& package_path,
                                          const std::string& output_directory);
}
