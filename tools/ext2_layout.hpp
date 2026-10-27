// Transient classification of a finalized native filesystem for LSMT packing.
#pragma once
#include "format/writer.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace obd::convert {
struct Ext2Layout {
    bool supported = false;
    std::string reason;
    std::vector<format::LsmtPayloadRange> metadata;
    uint64_t data_blocks = 0;
    uint64_t metadata_blocks = 0;
    uint64_t free_blocks = 0;
};
// Reads finalized ordinary-file bytes only. Unsupported features, inconsistent
// allocation or bounded-classification limits return an empty fallback plan.
Ext2Layout classify_ext2_layout(const std::string& path, uint64_t image_size);
}
