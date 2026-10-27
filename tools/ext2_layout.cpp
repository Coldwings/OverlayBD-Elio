#include "ext2_layout.hpp"
#include "format/lsmt_format.hpp"
#include <stdexcept>
#if OBD_HAVE_LIBE2FS
extern "C" {
#include <ext2fs/ext2fs.h>
}
#endif

namespace obd::convert {
#if OBD_HAVE_LIBE2FS
namespace {
void check(errcode_t code, const char* message) {
    if (code) throw std::runtime_error(std::string(message) + " (libe2fs error " +
                                     std::to_string(code) + ")");
}
struct Resources {
    ext2_filsys fs = nullptr;
    ext2fs_block_bitmap data = nullptr, structural = nullptr;
    ~Resources() {
        if (data) ext2fs_free_block_bitmap(data);
        if (structural) ext2fs_free_block_bitmap(structural);
        if (fs) ext2fs_free(fs); // Read-only analysis; no flush or image mutation.
    }
};
struct Walk {
    Resources* owner;
    uint64_t blocks;
    bool regular;
    bool invalid = false;
};
int visit(ext2_filsys fs, blk64_t* block, e2_blkcnt_t logical,
          blk64_t, int, void* context) {
    auto& walk = *static_cast<Walk*>(context);
    if (*block == 0) return 0;
    if (*block >= walk.blocks || !ext2fs_test_block_bitmap2(fs->block_map, *block)) {
        walk.invalid = true;
        return BLOCK_ABORT;
    }
    auto& owner = *walk.owner;
    if (walk.regular && logical >= 0) {
        // Duplicate claims remain conservative even for an inconsistent input.
        if (ext2fs_test_block_bitmap2(owner.data, *block))
            ext2fs_mark_block_bitmap2(owner.structural, *block);
        ext2fs_mark_block_bitmap2(owner.data, *block);
    } else {
        ext2fs_mark_block_bitmap2(owner.structural, *block);
    }
    return 0;
}
}
#endif

Ext2Layout classify_ext2_layout(const std::string& path, uint64_t image_size) {
    Ext2Layout result;
#if OBD_HAVE_LIBE2FS
    try {
        Resources owner;
        // The pinned fork's ext2fs_open additionally modifies lost+found.
        // Use the underlying open operation for read-only classification.
        check(ext2fs_open2(path.c_str(), nullptr, 0, 0, 0, unix_io_manager, &owner.fs),
              "cannot open finalized filesystem");
        const auto fs = owner.fs;
        auto* const super = fs->super;
        const uint64_t blocks = ext2fs_blocks_count(super);
        // Match the currently emitted native ext2 feature set. Other formats
        // remain byte-correct with the original logical-order fallback.
        if (fs->blocksize != 4096 || super->s_first_data_block != 0 ||
            (super->s_feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) ||
            (super->s_feature_ro_compat & ~(EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
                                           EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER)) ||
            (super->s_feature_compat & ~EXT2_FEATURE_COMPAT_EXT_ATTR))
            throw std::runtime_error("unsupported filesystem geometry or features");
        if (blocks == 0 || blocks > (uint64_t{1} << 26) ||
            super->s_inodes_count > (uint32_t{1} << 20) ||
            blocks * fs->blocksize != image_size)
            throw std::runtime_error("filesystem exceeds classification bounds");
        check(ext2fs_read_bitmaps(fs), "cannot read allocation bitmaps");
        check(ext2fs_allocate_block_bitmap(fs, "LSMT payload candidates", &owner.data),
              "cannot allocate payload classification");
        check(ext2fs_allocate_block_bitmap(fs, "LSMT structural blocks", &owner.structural),
              "cannot allocate structural classification");
        auto protect = [&](uint64_t first, uint64_t length) {
            if (first >= blocks || length == 0 || length > blocks - first)
                throw std::runtime_error("structural block range outside image");
            ext2fs_mark_block_bitmap_range2(owner.structural, first, static_cast<unsigned int>(length));
        };
        for (dgrp_t group = 0; group < fs->group_desc_count; ++group) {
            (void)ext2fs_reserve_super_and_bgd(fs, group, owner.structural);
            protect(ext2fs_block_bitmap_loc(fs, group), 1);
            protect(ext2fs_inode_bitmap_loc(fs, group), 1);
            protect(ext2fs_inode_table_loc(fs, group), fs->inode_blocks_per_group);
        }
        // Visit each allocated inode once: hardlink path multiplicity is not
        // duplicate ownership. Directory/symlink/reserved-inode data, indirect
        // blocks and external xattrs stay structural regardless of visit order.
        for (ext2_ino_t ino = 1; ino <= super->s_inodes_count; ++ino) {
            if (!ext2fs_test_inode_bitmap2(fs->inode_map, ino)) continue;
            struct ext2_inode inode{};
            check(ext2fs_read_inode(fs, ino, &inode), "cannot read allocated inode");
            const auto acl = ext2fs_file_acl_block(fs, &inode);
            if (acl) protect(acl, 1);
            if (!ext2fs_inode_has_valid_blocks2(fs, &inode)) continue;
            const bool regular = LINUX_S_ISREG(inode.i_mode) &&
                ino >= EXT2_FIRST_INODE(super) && inode.i_links_count != 0;
            Walk walk{&owner, blocks, regular};
            check(ext2fs_block_iterate3(fs, ino, BLOCK_FLAG_READ_ONLY, nullptr,
                                       visit, &walk), "cannot traverse inode block map");
            if (walk.invalid) throw std::runtime_error("inode references unallocated block");
        }
        for (uint64_t block = 0; block < blocks; ++block) {
            const bool allocated = ext2fs_test_block_bitmap2(fs->block_map, block);
            const bool structural = ext2fs_test_block_bitmap2(owner.structural, block);
            if (!allocated && !structural) { ++result.free_blocks; continue; }
            const bool data = ext2fs_test_block_bitmap2(owner.data, block);
            if (data && !structural) { ++result.data_blocks; continue; }
            ++result.metadata_blocks;
            const uint64_t sector = block * 8;
            if (!result.metadata.empty() &&
                result.metadata.back().offset + result.metadata.back().length == sector)
                result.metadata.back().length += 8;
            else {
                if (result.metadata.size() == format::lsmt::kMaxRoIndexSize)
                    throw std::runtime_error("classification exceeds index range budget");
                result.metadata.push_back({sector, 8});
            }
        }
        uint64_t index_entries = 0, logical = 0;
        constexpr uint64_t max_length = bytes::segment_mapping::kMaxLength;
        const auto count_entries = [&](uint64_t length) {
            return (length + max_length - 1) / max_length;
        };
        for (const auto& range : result.metadata) {
            index_entries += count_entries(range.offset - logical) + count_entries(range.length);
            logical = range.offset + range.length;
        }
        index_entries += count_entries(blocks * 8 - logical);
        if (index_entries > format::lsmt::kMaxRoIndexSize)
            throw std::runtime_error("classified placement exceeds index budget");
        result.supported = true;
        return result;
    } catch (const std::exception& error) {
        result = {};
        result.reason = error.what();
    }
#else
    (void)path;
    (void)image_size;
    result.reason = "libe2fs classification unavailable in this build";
#endif
    return result;
}
}
