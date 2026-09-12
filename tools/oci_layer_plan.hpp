#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace obd::convert {

enum class EntryKind { Regular, Directory, Symlink, Hardlink, Character, Block, Fifo };

struct PayloadSpan {
    uint64_t logical_offset = 0;
    uint64_t tar_offset = 0;  // byte offset in the original uncompressed tar
    uint64_t length = 0;
};

struct LayerEntry {
    std::string path;  // normalized archive-root-relative path; empty = root
    EntryKind kind = EntryKind::Regular;
    uint32_t mode = 0, uid = 0, gid = 0;
    int64_t mtime_seconds = 0;
    uint32_t mtime_nanoseconds = 0;
    std::string link_target;  // symlink text is retained verbatim
    uint64_t logical_size = 0;
    uint32_t device_major = 0, device_minor = 0;
    std::vector<PayloadSpan> payload_spans;
    std::map<std::string, std::string> xattrs;  // values may contain NUL
};

struct LayerPlan {
    std::vector<LayerEntry> entries;  // archive order, including replacements
    std::vector<std::string> whiteout_removals;
    std::vector<std::string> opaque_directories;
};

/// Parses without extraction or following archive paths on the host.
/// Supports USTAR, local/global PAX, GNU long names/links, GNU sparse PAX
/// 0.1 and 1.0. GNU sparse 0.0 and old GNU 'S' encodings are rejected.
/// Bounded to 65536 entries, 16 MiB per extension and 64 MiB retained
/// extension/path metadata, and one million sparse spans across the plan.
/// Throws obd::format_error for malformed/unsupported archives, obd::error
/// for input IO failures. Input must be an uncompressed regular tar file.
LayerPlan parse_oci_layer_plan(const std::string& tar_path);

}  // namespace obd::convert
