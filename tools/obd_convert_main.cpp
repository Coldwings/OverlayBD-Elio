// obd-convert: build a deterministic filesystem layer from a tar stream.
// The selected backend writes an ext2-compatible image directly (no mount, no
// device, no mkfs subprocess), then seals it through the LSMT writer.
#include "common/bytes.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include "format/writer.hpp"
#include "turbo_import.hpp"
#include "turbo_layered.hpp"
#include "format/lsmt_format.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if OBD_HAVE_LIBE2FS
extern "C" {
#include <et/com_err.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2_io.h>
#include <ext2fs/ext2fs.h>
}
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kBlockSize = 4096;
constexpr uint32_t kInodeSize = 128;
constexpr uint32_t kMaxBlocks = kBlockSize * 8;
constexpr uint32_t kMaxBuiltInInodes = kBlockSize * 8;
constexpr uint32_t kMaxBuiltInNodes = kMaxBuiltInInodes - 9;
constexpr uint32_t kFirstDynamicInode = 11;
constexpr uint64_t kMaxBuiltInDirectoryBlocks = 12;
constexpr uint64_t kMaxBuiltInFileBlocks = 12 + kBlockSize / 4;
constexpr uint64_t kMaxBuiltInFileBytes = kMaxBuiltInFileBlocks * kBlockSize;
constexpr uint32_t kMinBuiltInInodes = 128;
constexpr uint32_t kInodeTableBlock = 4;
constexpr uint64_t kExt2PointersPerBlock = kBlockSize / 4;
constexpr uint64_t kExt2BlocksPerGroup = kBlockSize * 8;
constexpr uint64_t kExt2MaxInodesPerGroup = kBlockSize * 8;
constexpr uint64_t kExt2GroupDescriptorSize = 32;
constexpr uint64_t kLibE2fsLastGroupSlackBlocks = 50;
constexpr uint64_t kExt2SectorsPerBlock = kBlockSize / 512;
constexpr uint64_t kMaxExt2IBlocksAllocationBlocks =
    std::numeric_limits<uint32_t>::max() / kExt2SectorsPerBlock;
constexpr uint32_t kExt2MaxLinks = 65535;
constexpr uint32_t kExt2MaxSubdirectories = kExt2MaxLinks - 2;
constexpr uint64_t kMaxExt2FileBlocks = 12 + kExt2PointersPerBlock +
                                       kExt2PointersPerBlock * kExt2PointersPerBlock +
                                       kExt2PointersPerBlock * kExt2PointersPerBlock *
                                           kExt2PointersPerBlock;
constexpr uint64_t regular_file_payload_blocks_for_data_blocks(uint64_t data_blocks) {
    uint64_t total = data_blocks;
    if (data_blocks <= 12) return total;

    data_blocks -= 12;
    ++total;
    if (data_blocks <= kExt2PointersPerBlock) return total;

    data_blocks -= kExt2PointersPerBlock;
    ++total;
    const uint64_t double_data_capacity = kExt2PointersPerBlock * kExt2PointersPerBlock;
    const uint64_t double_covered =
        data_blocks < double_data_capacity ? data_blocks : double_data_capacity;
    total += (double_covered + kExt2PointersPerBlock - 1) / kExt2PointersPerBlock;
    if (data_blocks <= double_data_capacity) return total;

    data_blocks -= double_data_capacity;
    ++total;
    total += (data_blocks + double_data_capacity - 1) / double_data_capacity;
    total += (data_blocks + kExt2PointersPerBlock - 1) / kExt2PointersPerBlock;
    return total;
}

constexpr uint64_t max_ext2_i_blocks_data_blocks() {
    uint64_t lo = 0;
    uint64_t hi = kMaxExt2FileBlocks;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo + 1) / 2;
        if (regular_file_payload_blocks_for_data_blocks(mid) <=
            kMaxExt2IBlocksAllocationBlocks) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

constexpr uint64_t kMaxLibE2fsFileBytes = max_ext2_i_blocks_data_blocks() * kBlockSize;
constexpr uint64_t kMaxLibE2fsDirectoryBlocks = 1024;
constexpr uint32_t kMaxLibE2fsNodes = 65536;
constexpr uint64_t kExt2DirectoryBaseEntryBytes = 24;
constexpr uint64_t kExt2DirectoryBaseMaxEntryBytes = 12;

constexpr uint16_t kExt2SIfReg = 0100000;
constexpr uint16_t kExt2SIfDir = 0040000;
constexpr uint16_t kExt2SIfLnk = 0120000;
constexpr uint32_t kExt2FeatureIncompatFiletype = 0x0002;
constexpr uint64_t kLibE2fsDefaultMinBytes = 4ull * 1024 * 1024;
constexpr uint64_t kLibE2fsPerNodeSlackBytes = 8192;
constexpr uint64_t kLibE2fsPerDirSlackBytes = 4096;

enum class ConverterBackend { BuiltinExt2, LibE2fs };

std::string_view backend_name(ConverterBackend backend) {
    switch (backend) {
    case ConverterBackend::BuiltinExt2: return "builtin-ext2";
    case ConverterBackend::LibE2fs: return "libe2fs";
    }
    return "unknown";
}

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct FdGuard {
    int fd = -1;
    FdGuard() = default;
    explicit FdGuard(int value) : fd(value) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& other) noexcept : fd(other.fd) { other.fd = -1; }
    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    ~FdGuard() { reset(); }
    void reset(int value = -1) noexcept {
        if (fd >= 0) ::close(fd);
        fd = value;
    }
    int release() noexcept {
        const int out = fd;
        fd = -1;
        return out;
    }
};

class TempWorkspace {
public:
    TempWorkspace(const std::string& out_dir, const std::string& stem) {
        std::string pattern = out_dir + "/." + stem + ".work.XXXXXX";
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        char* created = ::mkdtemp(buf.data());
        if (created == nullptr) {
            obd::throw_errno(errno, "cannot create temporary workspace in " + out_dir);
        }
        path_ = created;
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    ~TempWorkspace() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

uint64_t round_up(uint64_t value, uint64_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

uint64_t div_ceil(uint64_t n, uint64_t d) { return (n + d - 1) / d; }

void full_write(int fd, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t w = ::write(fd, p + done, size - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            obd::throw_errno(errno, "write failed");
        }
        if (w == 0) obd::throw_errno(EIO, "short write");
        done += static_cast<size_t>(w);
    }
}

void full_pwrite(int fd, const void* data, size_t size, uint64_t offset) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t w =
            ::pwrite(fd, p + done, size - done, static_cast<off_t>(offset + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            obd::throw_errno(errno, "pwrite failed");
        }
        if (w == 0) obd::throw_errno(EIO, "short pwrite");
        done += static_cast<size_t>(w);
    }
}

void full_pread(int fd, void* data, size_t size, uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t r =
            ::pread(fd, p + done, size - done, static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            obd::throw_errno(errno, "pread failed");
        }
        if (r == 0) obd::throw_errno(EIO, "unexpected EOF");
        done += static_cast<size_t>(r);
    }
}

bool read_exact(int fd, void* data, size_t size, bool allow_eof) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t r = ::read(fd, p + done, size - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            obd::throw_errno(errno, "tar read failed");
        }
        if (r == 0) {
            if (allow_eof && done == 0) return false;
            throw std::runtime_error("truncated tar stream");
        }
        done += static_cast<size_t>(r);
    }
    return true;
}

std::string sha256_of_fd(int fd, uint64_t size) {
    obd::common::Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t done = 0; done < size;) {
        const size_t chunk =
            static_cast<size_t>(std::min<uint64_t>(buf.size(), size - done));
        full_pread(fd, buf.data(), chunk, done);
        h.update(buf.data(), chunk);
        done += chunk;
    }
    return h.final_hex();
}

std::string sha256_of_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) obd::throw_errno(errno, "cannot open " + path);
    FdGuard guard(fd);
    struct stat st {};
    if (::fstat(fd, &st) != 0) obd::throw_errno(errno, "cannot stat " + path);
    return sha256_of_fd(fd, static_cast<uint64_t>(st.st_size));
}

uint64_t file_size(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) obd::throw_errno(errno, "cannot stat " + path);
    return static_cast<uint64_t>(st.st_size);
}

void fsync_file(const std::string& path, const char* what) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) obd::throw_errno(errno, std::string("cannot open ") + what + " " + path);
    FdGuard guard(fd);
    if (::fsync(fd) != 0) obd::throw_errno(errno, std::string("fsync failed for ") + what);
}

void fsync_directory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) obd::throw_errno(errno, "cannot open output directory " + path);
    FdGuard guard(fd);
    if (::fsync(fd) != 0) obd::throw_errno(errno, "fsync failed for output directory");
}

void replace_file_atomically(const std::string& src, const std::string& dst) {
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        obd::throw_errno(errno, "cannot publish " + dst);
    }
}

std::string uuid_from_digest(const std::string& digest_hex) {
    if (digest_hex.size() < 32) throw std::runtime_error("short sha256 digest");
    std::string u = digest_hex.substr(0, 32);
    u.insert(20, 1, '-');
    u.insert(16, 1, '-');
    u.insert(12, 1, '-');
    u.insert(8, 1, '-');
    return u;
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --input <rootfs.tar|-> --out-dir <dir> [--name base]\n"
                 "          [--backend builtin-ext2|libe2fs] [--size bytes] [--keep-raw]\n"
                 "          [--turboOCI] (local tar or gzip, libe2fs required)\n"
                 "          [--import-turboOCI package --descriptor descriptor.json [--parent-config config.json]]\n"
                 "          [--max-import-metadata-size bytes (default 1073741824)]\n"
                 "\n"
                 "Builds <out-dir>/<name>.lsmt from a ustar rootfs stream. Builds\n"
                 "with the pinned libe2fs backend use it by default; dependency-free\n"
                 "builds default to builtin-ext2. No device, mount, or mkfs subprocess\n"
                 "is used. JSON manifest metadata is printed to stdout. The built-in\n"
                 "backend supports regular files up\n"
                 "to 4,243,456 bytes, directories up to 12 data blocks,\n"
                 "short symlinks, uid/gid <= 65535,\n"
                 "at most 32768 inodes, and images up to 128 MiB.\n",
                 argv0);
}

uint64_t parse_size_arg(const std::string& s, const char* what) {
    if (s.empty() || s[0] == '-') {
        throw UsageError(std::string(what) + " must be a positive integer");
    }
    size_t pos = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(s, &pos, 10);
    } catch (const std::exception&) {
        throw UsageError(std::string(what) + " must be a positive integer");
    }
    if (pos != s.size() || value == 0) {
        throw UsageError(std::string(what) + " must be a positive integer");
    }
    return static_cast<uint64_t>(value);
}

uint64_t validate_explicit_image_blocks(uint64_t requested_size,
                                        ConverterBackend backend) {
    if (requested_size == 0) return 0;
    if (requested_size % kBlockSize != 0) {
        throw std::runtime_error("--size must be a multiple of 4096");
    }
    const uint64_t blocks = requested_size / kBlockSize;
    if (blocks == 0) throw std::runtime_error("--size must be non-zero");
    if (backend == ConverterBackend::BuiltinExt2 && blocks > kMaxBlocks) {
        throw std::runtime_error("built-in ext2 backend supports images up to 128 MiB");
    }
    return blocks;
}

uint64_t image_block_budget_for(uint64_t requested_size, ConverterBackend backend) {
    const uint64_t explicit_blocks = validate_explicit_image_blocks(requested_size, backend);
    if (backend == ConverterBackend::LibE2fs) {
        return explicit_blocks == 0 ? std::numeric_limits<uint64_t>::max() : explicit_blocks;
    }
    return explicit_blocks == 0 ? kMaxBlocks : explicit_blocks;
}

void validate_output_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        throw UsageError("--name must be a plain file stem");
    }
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) {
            throw UsageError("--name must contain only letters, digits, '.', '_' or '-'");
        }
    }
}

enum class NodeKind { Dir, File, Symlink };

struct Node {
    explicit Node(std::string n, NodeKind k, Node* p = nullptr)
        : name(std::move(n)),
          kind(k),
          parent(p),
          dir_blocks(k == NodeKind::Dir ? 1 : 0),
          dir_entry_bytes(k == NodeKind::Dir ? kExt2DirectoryBaseEntryBytes : 0),
          dir_entry_max_bytes(k == NodeKind::Dir ? kExt2DirectoryBaseMaxEntryBytes : 0) {}

    std::string name;
    NodeKind kind;
    Node* parent = nullptr;
    std::map<std::string, std::unique_ptr<Node>> children;
    uint16_t perm = 0755;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t size = 0;
    std::string spool_path;
    uint64_t tar_payload_offset = 0;
    std::string symlink_target;
    uint32_t inode = 0;
    uint32_t child_directory_count = 0;
    std::vector<uint32_t> blocks;
    uint32_t indirect_block = 0;
    uint64_t dir_blocks = 0;
    uint64_t dir_entry_bytes = 0;
    uint64_t dir_entry_max_bytes = 0;
    std::vector<uint8_t> dir_data;
};

struct BackendLimits {
    uint64_t max_file_bytes = kMaxBuiltInFileBytes;
    uint64_t max_directory_blocks = kMaxBuiltInDirectoryBlocks;
    uint64_t max_symlink_bytes = 60;
    uint64_t max_uid_gid = 65535;
    uint32_t max_nodes = kMaxBuiltInNodes;
    bool enforce_image_budget = true;
};

BackendLimits limits_for(ConverterBackend backend) {
    if (backend == ConverterBackend::LibE2fs) {
        return BackendLimits{
            kMaxLibE2fsFileBytes,
            kMaxLibE2fsDirectoryBlocks,
            4096,
            std::numeric_limits<uint32_t>::max(),
            kMaxLibE2fsNodes,
            true,
        };
    }
    return BackendLimits{};
}

uint8_t ext2_file_type(NodeKind kind) {
    switch (kind) {
    case NodeKind::File: return 1;
    case NodeKind::Dir: return 2;
    case NodeKind::Symlink: return 7;
    }
    return 0;
}

std::vector<std::string> split_tar_path(std::string path, NodeKind kind) {
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.rfind("./", 0) == 0) path.erase(0, 2);
    if (kind == NodeKind::Dir) {
        while (!path.empty() && path.back() == '/') path.pop_back();
    }
    if (path.empty() || path == ".") return {};
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t slash = path.find('/', pos);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string part = path.substr(pos, end - pos);
        if (part.empty() || part == "." || part == "..") {
            throw std::runtime_error("tar path escapes or contains an empty component: " + path);
        }
        if (part.size() > 255) {
            throw std::runtime_error("tar path component is longer than ext2 permits: " + part);
        }
        parts.push_back(part);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return parts;
}

size_t min_dirent_len(size_t name_len) { return static_cast<size_t>(round_up(8 + name_len, 4)); }

void throw_too_many_inodes(ConverterBackend backend) {
    if (backend == ConverterBackend::BuiltinExt2) {
        throw std::runtime_error("built-in ext2 backend supports at most 32768 inodes");
    }
    throw std::runtime_error("libe2fs backend supports at most 65536 in-memory tar nodes");
}

uint64_t parse_octal_field(const uint8_t* data, size_t size, const char* name) {
    size_t i = 0;
    while (i < size && (data[i] == ' ' || data[i] == '\0')) ++i;
    uint64_t value = 0;
    bool any = false;
    for (; i < size && data[i] != '\0' && data[i] != ' '; ++i) {
        if (data[i] < '0' || data[i] > '7') {
            throw std::runtime_error(std::string("unsupported tar ") + name + " field");
        }
        if (value > (std::numeric_limits<uint64_t>::max() >> 3)) {
            throw std::runtime_error(std::string("tar ") + name + " field is too large");
        }
        value = (value << 3) + static_cast<uint64_t>(data[i] - '0');
        any = true;
    }
    return any ? value : 0;
}

std::string tar_string(const uint8_t* data, size_t size) {
    size_t len = 0;
    while (len < size && data[len] != '\0') ++len;
    return std::string(reinterpret_cast<const char*>(data), len);
}

bool all_zero(const std::array<uint8_t, 512>& block) {
    return std::all_of(block.begin(), block.end(), [](uint8_t b) { return b == 0; });
}

void verify_tar_checksum(const std::array<uint8_t, 512>& header) {
    const uint64_t expected = parse_octal_field(header.data() + 148, 8, "checksum");
    uint64_t actual = 0;
    for (size_t i = 0; i < header.size(); ++i) {
        if (i >= 148 && i < 156) actual += static_cast<unsigned char>(' ');
        else actual += header[i];
    }
    if (expected != actual) throw std::runtime_error("tar header checksum mismatch");
}

void verify_ustar_header(const std::array<uint8_t, 512>& header) {
    if (std::memcmp(header.data() + 257, "ustar", 5) != 0 ||
        header[262] != '\0' ||
        std::memcmp(header.data() + 263, "00", 2) != 0) {
        throw std::runtime_error("unsupported tar header: expected ustar magic and version");
    }
}

uint64_t regular_file_payload_blocks(uint64_t size) {
    return regular_file_payload_blocks_for_data_blocks(div_ceil(size, kBlockSize));
}

uint32_t rounded_inode_count_for_nodes(uint32_t nodes, ConverterBackend backend) {
    if (backend == ConverterBackend::LibE2fs) {
        return static_cast<uint32_t>(
            round_up(std::max<uint32_t>(nodes + 32, kMinBuiltInInodes), 128));
    }
    const uint32_t used_inodes = 10 + (nodes - 1);
    uint32_t desired_inodes = std::max<uint32_t>(used_inodes + 32, kMinBuiltInInodes);
    desired_inodes = std::min<uint32_t>(desired_inodes, kMaxBuiltInInodes);
    return static_cast<uint32_t>(round_up(desired_inodes, 128));
}

struct LibE2fsGeometry {
    uint64_t groups = 1;
    uint64_t blocks_per_group = kExt2BlocksPerGroup;
    uint64_t group_descriptor_blocks = 1;
    uint64_t inode_table_blocks_per_group = 1;
};

LibE2fsGeometry libe2fs_geometry_for(uint32_t inode_count, uint64_t image_blocks) {
    const uint64_t blocks = std::max<uint64_t>(image_blocks, 1);
    uint64_t blocks_per_group = kExt2BlocksPerGroup;
    for (;;) {
        const uint64_t groups = std::max<uint64_t>(1, div_ceil(blocks, blocks_per_group));
        const uint64_t inodes_per_group = div_ceil(inode_count, groups);
        if (inodes_per_group <= kExt2MaxInodesPerGroup || blocks_per_group < 256) {
            const uint64_t inode_table_blocks_per_group = std::max<uint64_t>(
                1, div_ceil(inodes_per_group * kInodeSize, kBlockSize));
            const uint64_t group_descriptor_blocks = std::max<uint64_t>(
                1, div_ceil(groups * kExt2GroupDescriptorSize, kBlockSize));
            return LibE2fsGeometry{
                groups,
                blocks_per_group,
                group_descriptor_blocks,
                inode_table_blocks_per_group,
            };
        }
        blocks_per_group -= 8;
    }
}

struct LibE2fsMetadataBudget {
    uint64_t metadata_blocks = 0;
    uint64_t minimum_image_blocks = 0;
    uint64_t effective_image_blocks = 0;
};

uint64_t libe2fs_last_group_minimum_blocks(const LibE2fsGeometry& geometry) {
    return 3 + geometry.group_descriptor_blocks + geometry.inode_table_blocks_per_group +
           kLibE2fsLastGroupSlackBlocks;
}

uint64_t libe2fs_effective_image_blocks(uint32_t inode_count, uint64_t image_blocks) {
    if (image_blocks == 0) return 0;
    uint64_t effective_blocks = image_blocks;
    for (int i = 0; i < 16; ++i) {
        const auto geometry = libe2fs_geometry_for(inode_count, effective_blocks);
        const uint64_t tail_blocks = effective_blocks % geometry.blocks_per_group;
        if (tail_blocks == 0) return effective_blocks;
        if (tail_blocks >= libe2fs_last_group_minimum_blocks(geometry)) {
            return effective_blocks;
        }
        if (tail_blocks == effective_blocks) return 0;
        effective_blocks -= tail_blocks;
    }
    return effective_blocks;
}

LibE2fsMetadataBudget libe2fs_metadata_budget(uint32_t inode_count,
                                              uint64_t image_blocks) {
    const uint64_t effective_image_blocks =
        libe2fs_effective_image_blocks(inode_count, image_blocks);
    const uint64_t geometry_image_blocks =
        effective_image_blocks == 0 ? image_blocks : effective_image_blocks;
    const auto geometry = libe2fs_geometry_for(inode_count, geometry_image_blocks);

    LibE2fsMetadataBudget budget;
    budget.effective_image_blocks = effective_image_blocks;
    budget.metadata_blocks =
        geometry.groups *
        (3 + geometry.group_descriptor_blocks + geometry.inode_table_blocks_per_group);

    // ext2fs_initialize() drops a short final group and fails only when that
    // leaves no usable group. Explicit --size preflight compares content with
    // effective_image_blocks; auto sizing uses minimum_image_blocks to grow
    // away from sizes that libe2fs would otherwise shorten.
    const uint64_t last_group_minimum = libe2fs_last_group_minimum_blocks(geometry);
    if (image_blocks == 0) {
        budget.minimum_image_blocks = last_group_minimum;
    } else {
        const uint64_t tail_blocks = image_blocks % geometry.blocks_per_group;
        if (tail_blocks != 0 && tail_blocks < last_group_minimum) {
            budget.minimum_image_blocks =
                image_blocks - tail_blocks + last_group_minimum;
        }
    }
    return budget;
}

uint64_t metadata_blocks_for_nodes(uint32_t nodes, ConverterBackend backend,
                                   uint64_t image_blocks = 0) {
    const uint32_t inode_count = rounded_inode_count_for_nodes(nodes, backend);
    const uint64_t inode_table_blocks =
        div_ceil(static_cast<uint64_t>(inode_count) * kInodeSize, kBlockSize);
    if (backend == ConverterBackend::BuiltinExt2) return kInodeTableBlock + inode_table_blocks;
    return libe2fs_metadata_budget(inode_count, image_blocks).metadata_blocks;
}

[[maybe_unused]] uint64_t minimum_libe2fs_image_blocks_for_nodes(uint32_t nodes,
                                                                 uint64_t image_blocks) {
    return libe2fs_metadata_budget(rounded_inode_count_for_nodes(nodes, ConverterBackend::LibE2fs),
                                   image_blocks)
        .minimum_image_blocks;
}

uint64_t effective_libe2fs_image_blocks_for_nodes(uint32_t nodes, uint64_t image_blocks) {
    return libe2fs_metadata_budget(rounded_inode_count_for_nodes(nodes, ConverterBackend::LibE2fs),
                                   image_blocks)
        .effective_image_blocks;
}

uint64_t directory_payload_blocks_for_data_blocks(uint64_t data_blocks) {
    return regular_file_payload_blocks(data_blocks * kBlockSize);
}

uint64_t directory_blocks_for_entry_summary(uint64_t entry_bytes,
                                            uint64_t max_entry_bytes) {
    if (entry_bytes == 0) return 0;
    const uint64_t guaranteed_payload_per_block =
        kBlockSize - std::min<uint64_t>(max_entry_bytes - 1, kBlockSize - 1);
    return div_ceil(entry_bytes, guaranteed_payload_per_block);
}

uint64_t directory_blocks_sorted(const Node& dir, std::string_view pending_name,
                                 bool has_pending) {
    uint64_t blocks = 1;
    size_t used = 0;
    auto add_entry = [&](size_t name_len) {
        const size_t need = min_dirent_len(name_len);
        if (used != 0 && used + need > kBlockSize) {
            ++blocks;
            used = 0;
        }
        used += need;
    };
    add_entry(1);  // "."
    add_entry(2);  // ".."
    bool inserted_pending = false;
    for (const auto& [name, _] : dir.children) {
        if (has_pending && !inserted_pending && pending_name < std::string_view(name)) {
            add_entry(pending_name.size());
            inserted_pending = true;
        }
        add_entry(name.size());
    }
    if (has_pending && !inserted_pending) add_entry(pending_name.size());
    return blocks;
}

uint64_t directory_blocks_with_pending_child(const Node& dir,
                                             std::string_view pending_name) {
    return directory_blocks_sorted(dir, pending_name, true);
}

uint64_t exact_directory_blocks(const Node& dir) {
    return directory_blocks_sorted(dir, {}, false);
}

struct TarReader {
    int fd;
    std::string work_dir;
    std::string stem;
    ConverterBackend backend = ConverterBackend::BuiltinExt2;
    BackendLimits limits;
    uint64_t max_image_blocks = kMaxBlocks;
    std::vector<std::string> spool_paths;
    uint64_t next_spool = 0;
    uint64_t stream_offset = 0;
    uint64_t reserved_payload_blocks = 0;
    uint64_t reserved_directory_blocks = directory_payload_blocks_for_data_blocks(1);
    uint32_t node_count = 1;

    TarReader(int input_fd, std::string workspace_dir, std::string output_stem,
              ConverterBackend selected_backend, uint64_t image_block_budget)
        : fd(input_fd),
          work_dir(std::move(workspace_dir)),
          stem(std::move(output_stem)),
          backend(selected_backend),
          limits(limits_for(selected_backend)),
          max_image_blocks(image_block_budget) {}

    ~TarReader() {
        for (const auto& path : spool_paths) ::unlink(path.c_str());
    }

    std::string make_spool_path() {
        return work_dir + "/" + stem + ".entry-" + std::to_string(next_spool++) + ".tmp";
    }

    bool read_tar_exact(void* buf, size_t count, bool allow_eof) {
        if (count > std::numeric_limits<uint64_t>::max() - stream_offset)
            throw std::runtime_error("tar offset overflow");
        if (!read_exact(fd, buf, count, allow_eof)) return false;
        stream_offset += count;
        return true;
    }

    void skip_padding(uint64_t payload_size) {
        const uint64_t pad = (512 - (payload_size % 512)) % 512;
        std::array<uint8_t, 512> discard {};
        if (pad != 0) read_tar_exact(discard.data(), static_cast<size_t>(pad), false);
    }

    std::string spool_payload(uint64_t payload_size) {
        const std::string path = make_spool_path();
        const int out =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (out < 0) obd::throw_errno(errno, "cannot create payload spool " + path);
        FdGuard guard(out);
        spool_paths.push_back(path);
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t left = payload_size; left > 0;) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buf.size(), left));
            read_tar_exact(buf.data(), chunk, false);
            full_write(out, buf.data(), chunk);
            left -= chunk;
        }
        return path;
    }

    std::runtime_error image_budget_error(const std::string& name) const {
        return std::runtime_error(
            std::string(backend_name(backend)) +
            " backend tar contents and ext2 metadata exceed image budget: " + name);
    }

    uint64_t required_image_blocks(uint64_t payload_blocks) const {
        const uint64_t image_blocks =
            max_image_blocks == std::numeric_limits<uint64_t>::max() ? 0 : max_image_blocks;
        if (backend == ConverterBackend::LibE2fs && image_blocks != 0) {
            const uint64_t effective_blocks =
                effective_libe2fs_image_blocks_for_nodes(node_count, image_blocks);
            if (effective_blocks == 0) return max_image_blocks + 1;
            const uint64_t content_blocks =
                metadata_blocks_for_nodes(node_count, backend, effective_blocks) +
                reserved_directory_blocks + payload_blocks;
            return content_blocks > effective_blocks ? max_image_blocks + 1 : content_blocks;
        }
        const uint64_t content_blocks =
            metadata_blocks_for_nodes(node_count, backend, image_blocks) +
            reserved_directory_blocks + payload_blocks;
        return content_blocks;
    }

    void ensure_image_budget(const std::string& name) const {
        if (limits.enforce_image_budget &&
            required_image_blocks(reserved_payload_blocks) > max_image_blocks) {
            throw image_budget_error(name);
        }
    }

    std::string directory_display_name(const Node& dir) const {
        return dir.name.empty() ? std::string("/") : dir.name;
    }

    void throw_directory_too_large(const Node& dir) const {
        if (backend == ConverterBackend::BuiltinExt2) {
            throw std::runtime_error(
                "built-in ext2 backend supports directories up to 12 data blocks: " +
                directory_display_name(dir));
        }
        throw std::runtime_error(
            std::string(backend_name(backend)) +
            " backend directory exceeds supported size: " + directory_display_name(dir));
    }

    void adjust_reserved_directory_blocks(uint64_t old_blocks, uint64_t new_blocks) {
        const uint64_t old_payload = directory_payload_blocks_for_data_blocks(old_blocks);
        const uint64_t new_payload = directory_payload_blocks_for_data_blocks(new_blocks);
        if (new_payload >= old_payload) {
            reserved_directory_blocks += new_payload - old_payload;
        } else {
            reserved_directory_blocks -= old_payload - new_payload;
        }
    }

    void set_directory_blocks(Node& dir, uint64_t next_blocks) {
        adjust_reserved_directory_blocks(dir.dir_blocks, next_blocks);
        dir.dir_blocks = next_blocks;
    }

    uint64_t exact_reserved_directory_blocks(Node& dir) const {
        if (dir.kind != NodeKind::Dir) return 0;
        const uint64_t blocks = exact_directory_blocks(dir);
        if (blocks > limits.max_directory_blocks) throw_directory_too_large(dir);
        dir.dir_blocks = blocks;
        uint64_t reserved = directory_payload_blocks_for_data_blocks(blocks);
        for (const auto& [_, child] : dir.children) {
            reserved += exact_reserved_directory_blocks(*child);
        }
        return reserved;
    }

    void finalize_directory_accounting(Node& root) {
        reserved_directory_blocks = exact_reserved_directory_blocks(root);
        ensure_image_budget("/");
    }

    void reserve_directory_child(Node& parent, const std::string& leaf,
                                 bool child_is_directory) {
        if (child_is_directory && parent.child_directory_count >= kExt2MaxSubdirectories) {
            throw std::runtime_error(
                std::string(backend_name(backend)) +
                " backend directory has too many child directories for ext2 link count: " +
                directory_display_name(parent));
        }

        const uint64_t entry_bytes = min_dirent_len(leaf.size());
        const uint64_t next_entry_bytes = parent.dir_entry_bytes + entry_bytes;
        const uint64_t next_max_entry_bytes =
            std::max<uint64_t>(parent.dir_entry_max_bytes, entry_bytes);
        uint64_t next_blocks =
            directory_blocks_for_entry_summary(next_entry_bytes, next_max_entry_bytes);
        bool exact = false;
        if (next_blocks > limits.max_directory_blocks) {
            next_blocks = directory_blocks_with_pending_child(parent, leaf);
            exact = true;
            if (next_blocks > limits.max_directory_blocks) throw_directory_too_large(parent);
        }

        set_directory_blocks(parent, next_blocks);
        parent.dir_entry_bytes = next_entry_bytes;
        parent.dir_entry_max_bytes = next_max_entry_bytes;
        if (child_is_directory) {
            ++parent.child_directory_count;
            reserved_directory_blocks += directory_payload_blocks_for_data_blocks(1);
        }

        if (!exact && limits.enforce_image_budget &&
            required_image_blocks(reserved_payload_blocks) > max_image_blocks) {
            const uint64_t exact_blocks = directory_blocks_with_pending_child(parent, leaf);
            if (exact_blocks > limits.max_directory_blocks) throw_directory_too_large(parent);
            set_directory_blocks(parent, exact_blocks);
        }
    }

    Node& ensure_dir(Node& root, const std::vector<std::string>& parts) {
        Node* cur = &root;
        for (const auto& part : parts) {
            auto it = cur->children.find(part);
            if (it == cur->children.end()) {
                if (node_count >= limits.max_nodes) throw_too_many_inodes(backend);
                ++node_count;
                reserve_directory_child(*cur, part, true);
                auto dir = std::make_unique<Node>(part, NodeKind::Dir, cur);
                ensure_image_budget(part);
                it = cur->children.emplace(part, std::move(dir)).first;
            }
            if (it->second->kind != NodeKind::Dir) {
                throw std::runtime_error("tar path parent is not a directory: " + part);
            }
            cur = it->second.get();
        }
        return *cur;
    }

    void reserve_payload_blocks(uint64_t blocks, const std::string& name) {
        if (!limits.enforce_image_budget || blocks == 0) return;
        if (blocks > max_image_blocks || reserved_payload_blocks > max_image_blocks - blocks) {
            throw image_budget_error(name);
        }
        const uint64_t reserved_after = reserved_payload_blocks + blocks;
        const uint64_t required_blocks = required_image_blocks(reserved_after);
        if (required_blocks > max_image_blocks) {
            throw image_budget_error(name);
        }
        reserved_payload_blocks = reserved_after;
    }

    void reserve_regular_file_payload(uint64_t size, const std::string& name) {
        reserve_payload_blocks(regular_file_payload_blocks(size), name);
    }

    void reserve_symlink_payload(uint64_t target_size, const std::string& name) {
        const bool needs_data_block =
            backend == ConverterBackend::LibE2fs ? target_size >= 60 : target_size > 60;
        if (!needs_data_block) return;
        reserve_payload_blocks(div_ceil(target_size, kBlockSize), name);
    }

    void load_into(Node& root) {
        bool saw_entry = false;
        for (;;) {
            std::array<uint8_t, 512> header {};
            if (!read_tar_exact(header.data(), header.size(), true)) {
                if (!saw_entry) throw std::runtime_error("empty tar stream");
                throw std::runtime_error("tar stream missing end-of-archive marker");
            }
            if (all_zero(header)) {
                std::array<uint8_t, 512> second {};
                if (!read_tar_exact(second.data(), second.size(), true) ||
                    !all_zero(second)) {
                    throw std::runtime_error(
                        "tar end-of-archive requires two zero blocks");
                }
                if (!saw_entry) throw std::runtime_error("empty tar stream");
                break;
            }
            saw_entry = true;
            verify_tar_checksum(header);
            verify_ustar_header(header);

            std::string name = tar_string(header.data(), 100);
            const std::string prefix = tar_string(header.data() + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
            const char typeflag = header[156] == '\0' ? '0' : static_cast<char>(header[156]);
            const uint64_t mode = parse_octal_field(header.data() + 100, 8, "mode");
            const uint64_t uid = parse_octal_field(header.data() + 108, 8, "uid");
            const uint64_t gid = parse_octal_field(header.data() + 116, 8, "gid");
            const uint64_t size = parse_octal_field(header.data() + 124, 12, "size");
            if (uid > limits.max_uid_gid || gid > limits.max_uid_gid) {
                throw std::runtime_error(
                    std::string(backend_name(backend)) +
                    " backend uid/gid exceeds supported range");
            }
            const uint16_t perm = static_cast<uint16_t>(mode & 07777);

            if (typeflag == '5') {
                if (size != 0) throw std::runtime_error("directory tar entry has a payload");
                auto parts = split_tar_path(name, NodeKind::Dir);
                Node& dir = ensure_dir(root, parts);
                dir.perm = perm;
                dir.uid = static_cast<uint32_t>(uid);
                dir.gid = static_cast<uint32_t>(gid);
                continue;
            }

            if (typeflag != '0' && typeflag != '2') {
                throw std::runtime_error("unsupported tar entry type: " + std::string(1, typeflag));
            }
            const NodeKind kind = typeflag == '2' ? NodeKind::Symlink : NodeKind::File;
            auto parts = split_tar_path(name, kind);
            if (parts.empty()) throw std::runtime_error("tar entry names the root directory as a file");
            std::vector<std::string> parent_parts(parts.begin(), parts.end() - 1);
            Node& parent = ensure_dir(root, parent_parts);
            const std::string leaf = parts.back();
            if (parent.children.count(leaf) != 0) {
                throw std::runtime_error("duplicate tar entry: " + name);
            }
            if (node_count >= limits.max_nodes) throw_too_many_inodes(backend);
            ++node_count;
            reserve_directory_child(parent, leaf, kind == NodeKind::Dir);
            ensure_image_budget(name);
            auto node = std::make_unique<Node>(leaf, kind, &parent);
            node->perm = perm;
            node->uid = static_cast<uint32_t>(uid);
            node->gid = static_cast<uint32_t>(gid);
            if (kind == NodeKind::File) {
                if (size > limits.max_file_bytes) {
                    throw std::runtime_error(std::string(backend_name(backend)) +
                                             " backend file is too large: " + name);
                }
                // Reserve declared ext2 payload blocks, directory blocks, and
                // inode metadata before reading bytes so an oversized archive
                // cannot fill the temporary workspace and fail only after
                // final image sizing.
                reserve_regular_file_payload(size, name);
                node->size = size;
                node->tar_payload_offset = stream_offset;
                if (size != 0) node->spool_path = spool_payload(size);
                skip_padding(size);
            } else {
                if (size != 0) throw std::runtime_error("symlink tar entry has a payload");
                node->symlink_target = tar_string(header.data() + 157, 100);
                if (node->symlink_target.empty()) {
                    throw std::runtime_error("symlink tar entry has an empty target");
                }
                if (node->symlink_target.size() > limits.max_symlink_bytes) {
                    throw std::runtime_error(std::string(backend_name(backend)) +
                                             " backend symlink target is too long");
                }
                reserve_symlink_payload(node->symlink_target.size(), name);
                node->size = node->symlink_target.size();
            }
            parent.children.emplace(leaf, std::move(node));
        }
        finalize_directory_accounting(root);
    }
};

uint32_t count_nodes(const Node& node) {
    uint32_t n = 1;
    for (const auto& [_, child] : node.children) n += count_nodes(*child);
    return n;
}

uint32_t count_dirs(const Node& node) {
    uint32_t n = node.kind == NodeKind::Dir ? 1 : 0;
    for (const auto& [_, child] : node.children) n += count_dirs(*child);
    return n;
}

void assign_inodes(Node& root) {
    root.inode = 2;
    uint32_t next = kFirstDynamicInode;
    auto walk = [&](auto&& self, Node& dir) -> void {
        for (auto& [_, child] : dir.children) {
            child->inode = next++;
            if (child->kind == NodeKind::Dir) self(self, *child);
        }
    };
    walk(walk, root);
}

void put_dirent(std::vector<uint8_t>& block, size_t offset, uint32_t ino,
                uint16_t rec_len, uint8_t file_type, std::string_view name) {
    obd::bytes::store_u32_le(block.data() + offset, ino);
    obd::bytes::store_u16_le(block.data() + offset + 4, rec_len);
    block[offset + 6] = static_cast<uint8_t>(name.size());
    block[offset + 7] = file_type;
    std::memcpy(block.data() + offset + 8, name.data(), name.size());
}

std::vector<uint8_t> build_dir_data(const Node& dir) {
    struct Entry { uint32_t ino; uint8_t type; std::string name; };
    std::vector<Entry> entries;
    entries.push_back({dir.inode, 2, "."});
    entries.push_back({dir.parent ? dir.parent->inode : dir.inode, 2, ".."});
    for (const auto& [name, child] : dir.children) {
        entries.push_back({child->inode, ext2_file_type(child->kind), name});
    }

    std::vector<uint8_t> out;
    std::vector<uint8_t> block(kBlockSize, 0);
    size_t used = 0;
    size_t last_off = 0;
    for (const auto& entry : entries) {
        const size_t need = min_dirent_len(entry.name.size());
        if (used != 0 && used + need > kBlockSize) {
            const uint16_t old_len = obd::bytes::load_u16_le(block.data() + last_off + 4);
            obd::bytes::store_u16_le(block.data() + last_off + 4,
                                     static_cast<uint16_t>(old_len + kBlockSize - used));
            out.insert(out.end(), block.begin(), block.end());
            std::fill(block.begin(), block.end(), 0);
            used = 0;
        }
        last_off = used;
        put_dirent(block, used, entry.ino, static_cast<uint16_t>(need), entry.type, entry.name);
        used += need;
    }
    if (used == 0) throw std::runtime_error("internal error: empty directory block");
    const uint16_t old_len = obd::bytes::load_u16_le(block.data() + last_off + 4);
    obd::bytes::store_u16_le(block.data() + last_off + 4,
                             static_cast<uint16_t>(old_len + kBlockSize - used));
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

void build_directory_payloads(Node& node) {
    if (node.kind == NodeKind::Dir) {
        node.dir_data = build_dir_data(node);
        const uint64_t blocks = node.dir_data.size() / kBlockSize;
        if (blocks > kMaxBuiltInDirectoryBlocks) {
            throw std::runtime_error(
                "built-in ext2 backend supports directories up to 12 data blocks: " +
                (node.name.empty() ? std::string("/") : node.name));
        }
        node.size = node.dir_data.size();
        for (auto& [_, child] : node.children) build_directory_payloads(*child);
    }
}

uint64_t file_data_blocks(const Node& node) {
    if (node.kind == NodeKind::File) return div_ceil(node.size, kBlockSize);
    if (node.kind == NodeKind::Dir) return node.dir_data.size() / kBlockSize;
    return 0;
}

uint64_t file_indirect_blocks(const Node& node) {
    return node.kind == NodeKind::File && file_data_blocks(node) > 12 ? 1 : 0;
}

uint64_t count_payload_blocks(const Node& node) {
    uint64_t blocks = file_data_blocks(node) + file_indirect_blocks(node);
    for (const auto& [_, child] : node.children) blocks += count_payload_blocks(*child);
    return blocks;
}

void allocate_blocks(Node& node, uint32_t& next_block) {
    const uint64_t data_blocks = file_data_blocks(node);
    if (node.kind == NodeKind::Dir && data_blocks > kMaxBuiltInDirectoryBlocks) {
        throw std::runtime_error(
            "built-in ext2 backend supports directories up to 12 data blocks: " +
            (node.name.empty() ? std::string("/") : node.name));
    }
    if (node.kind == NodeKind::File && data_blocks > 12) node.indirect_block = next_block++;
    for (uint64_t i = 0; i < data_blocks; ++i) node.blocks.push_back(next_block++);
    for (auto& [_, child] : node.children) allocate_blocks(*child, next_block);
}

uint32_t dir_link_count(const Node& dir) {
    uint32_t links = 2;
    for (const auto& [_, child] : dir.children) {
        if (child->kind == NodeKind::Dir) ++links;
    }
    return links;
}

void store_inode(std::vector<uint8_t>& inode, const Node& node) {
    std::fill(inode.begin(), inode.end(), 0);
    uint16_t type = 0;
    uint16_t links = 1;
    if (node.kind == NodeKind::Dir) {
        type = kExt2SIfDir;
        links = static_cast<uint16_t>(dir_link_count(node));
    } else if (node.kind == NodeKind::File) {
        type = kExt2SIfReg;
    } else {
        type = kExt2SIfLnk;
    }
    obd::bytes::store_u16_le(inode.data() + 0, static_cast<uint16_t>(type | node.perm));
    obd::bytes::store_u16_le(inode.data() + 2, static_cast<uint16_t>(node.uid));
    obd::bytes::store_u32_le(inode.data() + 4, static_cast<uint32_t>(node.size));
    obd::bytes::store_u16_le(inode.data() + 24, static_cast<uint16_t>(node.gid));
    obd::bytes::store_u16_le(inode.data() + 26, links);
    const uint32_t sectors = static_cast<uint32_t>(
        (node.blocks.size() + (node.indirect_block != 0 ? 1 : 0)) * (kBlockSize / 512));
    obd::bytes::store_u32_le(inode.data() + 28, sectors);
    if (node.kind == NodeKind::Symlink) {
        std::memcpy(inode.data() + 40, node.symlink_target.data(), node.symlink_target.size());
        return;
    }
    for (size_t i = 0; i < node.blocks.size() && i < 12; ++i) {
        obd::bytes::store_u32_le(inode.data() + 40 + i * 4, node.blocks[i]);
    }
    if (node.indirect_block != 0) {
        obd::bytes::store_u32_le(inode.data() + 40 + 12 * 4, node.indirect_block);
    }
}

void set_bitmap_bit(std::vector<uint8_t>& bitmap, uint32_t index) {
    if (index / 8 >= bitmap.size()) {
        throw std::runtime_error("internal ext2 bitmap index is out of range");
    }
    bitmap[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

void write_node_inode(int fd, const Node& node, uint32_t inode_table_block) {
    std::vector<uint8_t> inode(kInodeSize, 0);
    store_inode(inode, node);
    const uint64_t offset = static_cast<uint64_t>(inode_table_block) * kBlockSize +
                            static_cast<uint64_t>(node.inode - 1) * kInodeSize;
    full_pwrite(fd, inode.data(), inode.size(), offset);
    for (const auto& [_, child] : node.children) write_node_inode(fd, *child, inode_table_block);
}

void write_file_payload(int out_fd, const Node& node) {
    if (node.kind == NodeKind::Dir) {
        for (size_t i = 0; i < node.blocks.size(); ++i) {
            full_pwrite(out_fd, node.dir_data.data() + i * kBlockSize, kBlockSize,
                        static_cast<uint64_t>(node.blocks[i]) * kBlockSize);
        }
    } else if (node.kind == NodeKind::File) {
        if (node.indirect_block != 0) {
            std::vector<uint8_t> indirect(kBlockSize, 0);
            for (size_t i = 12; i < node.blocks.size(); ++i) {
                obd::bytes::store_u32_le(indirect.data() + (i - 12) * 4, node.blocks[i]);
            }
            full_pwrite(out_fd, indirect.data(), indirect.size(),
                        static_cast<uint64_t>(node.indirect_block) * kBlockSize);
        }
        if (node.size != 0) {
            const int in = ::open(node.spool_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (in < 0) obd::throw_errno(errno, "cannot open payload spool " + node.spool_path);
            FdGuard guard(in);
            std::vector<uint8_t> block(kBlockSize, 0);
            uint64_t remaining = node.size;
            for (uint32_t b : node.blocks) {
                std::fill(block.begin(), block.end(), 0);
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(kBlockSize, remaining));
                if (chunk > 0) read_exact(in, block.data(), chunk, false);
                full_pwrite(out_fd, block.data(), block.size(),
                            static_cast<uint64_t>(b) * kBlockSize);
                remaining -= chunk;
            }
        }
    }
    for (const auto& [_, child] : node.children) write_file_payload(out_fd, *child);
}

#if OBD_HAVE_LIBE2FS
uint64_t count_libe2fs_payload_blocks(const Node& node) {
    uint64_t total = 0;
    if (node.kind == NodeKind::File) total += regular_file_payload_blocks(node.size);
    if (node.kind == NodeKind::Symlink && node.size >= 60) {
        total += div_ceil(node.size, kBlockSize);
    }
    for (const auto& [_, child] : node.children) total += count_libe2fs_payload_blocks(*child);
    return total;
}

uint64_t auto_libe2fs_size_bytes(const Node& root) {
    const uint64_t payload_blocks = count_libe2fs_payload_blocks(root);
    const uint64_t nodes = count_nodes(root);
    const uint64_t dirs = count_dirs(root);
    const uint64_t slack = nodes * kLibE2fsPerNodeSlackBytes + dirs * kLibE2fsPerDirSlackBytes +
                           kLibE2fsDefaultMinBytes;
    uint64_t estimate = round_up(
        std::max<uint64_t>(payload_blocks * kBlockSize + slack, kLibE2fsDefaultMinBytes),
        kBlockSize);
    for (int i = 0; i < 4; ++i) {
        const uint64_t estimate_blocks = estimate / kBlockSize;
        const uint64_t metadata_blocks =
            metadata_blocks_for_nodes(static_cast<uint32_t>(nodes), ConverterBackend::LibE2fs,
                                      estimate_blocks);
        const uint64_t content_required_blocks = payload_blocks + metadata_blocks;
        const uint64_t required_blocks = std::max(
            content_required_blocks,
            minimum_libe2fs_image_blocks_for_nodes(static_cast<uint32_t>(nodes), estimate_blocks));
        const uint64_t next = round_up(std::max<uint64_t>(required_blocks * kBlockSize + slack,
                                                          kLibE2fsDefaultMinBytes),
                                       kBlockSize);
        if (next == estimate) break;
        estimate = next;
    }
    return estimate;
}
#endif

void write_ext2_image(Node& root, const std::string& path, uint64_t requested_size) {
    assign_inodes(root);
    build_directory_payloads(root);

    const uint32_t nodes = count_nodes(root);
    const uint32_t used_inodes = 10 + (nodes - 1);
    if (used_inodes > kMaxBuiltInInodes) {
        throw_too_many_inodes(ConverterBackend::BuiltinExt2);
    }
    uint32_t desired_inodes = std::max<uint32_t>(used_inodes + 32, kMinBuiltInInodes);
    desired_inodes = std::min<uint32_t>(desired_inodes, kMaxBuiltInInodes);
    const uint32_t inode_count =
        static_cast<uint32_t>(round_up(desired_inodes, 128));
    const uint32_t inode_table_blocks = static_cast<uint32_t>(div_ceil(
        static_cast<uint64_t>(inode_count) * kInodeSize, kBlockSize));
    const uint32_t inode_table_block = kInodeTableBlock;
    const uint32_t first_data_block = inode_table_block + inode_table_blocks;
    const uint64_t payload_blocks = count_payload_blocks(root);
    const uint64_t required_blocks = first_data_block + payload_blocks;

    uint64_t total_blocks = 0;
    if (requested_size != 0) {
        total_blocks = validate_explicit_image_blocks(requested_size, ConverterBackend::BuiltinExt2);
        if (total_blocks < required_blocks) {
            throw std::runtime_error("--size is too small for tar contents and ext2 metadata");
        }
    } else {
        total_blocks = round_up(std::max<uint64_t>(required_blocks, 1024), 256);
    }
    if (total_blocks == 0 || total_blocks > kMaxBlocks) {
        throw std::runtime_error("built-in ext2 backend supports images up to 128 MiB");
    }

    uint32_t next_block = first_data_block;
    allocate_blocks(root, next_block);
    if (next_block > total_blocks) throw std::runtime_error("internal ext2 block allocation overflow");
    const uint32_t used_blocks = next_block;
    const uint32_t free_blocks = static_cast<uint32_t>(total_blocks - used_blocks);
    const uint32_t free_inodes = inode_count - used_inodes;

    const int fd =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) obd::throw_errno(errno, "cannot create raw filesystem " + path);
    FdGuard guard(fd);
    if (::ftruncate(fd, static_cast<off_t>(total_blocks * kBlockSize)) != 0) {
        obd::throw_errno(errno, "cannot size raw filesystem " + path);
    }

    std::vector<uint8_t> super(1024, 0);
    obd::bytes::store_u32_le(super.data() + 0, inode_count);
    obd::bytes::store_u32_le(super.data() + 4, static_cast<uint32_t>(total_blocks));
    obd::bytes::store_u32_le(super.data() + 8, 0);
    obd::bytes::store_u32_le(super.data() + 12, free_blocks);
    obd::bytes::store_u32_le(super.data() + 16, free_inodes);
    obd::bytes::store_u32_le(super.data() + 20, 0);
    obd::bytes::store_u32_le(super.data() + 24, 2);
    obd::bytes::store_u32_le(super.data() + 28, 2);
    obd::bytes::store_u32_le(super.data() + 32, kMaxBlocks);
    obd::bytes::store_u32_le(super.data() + 36, kMaxBlocks);
    obd::bytes::store_u32_le(super.data() + 40, inode_count);
    obd::bytes::store_u16_le(super.data() + 52, 0);
    obd::bytes::store_u16_le(super.data() + 54, 0xffff);
    obd::bytes::store_u16_le(super.data() + 56, 0xef53);
    obd::bytes::store_u16_le(super.data() + 58, 1);
    obd::bytes::store_u16_le(super.data() + 60, 1);
    obd::bytes::store_u32_le(super.data() + 76, 1);
    obd::bytes::store_u32_le(super.data() + 84, kFirstDynamicInode);
    obd::bytes::store_u16_le(super.data() + 88, kInodeSize);
    obd::bytes::store_u32_le(super.data() + 96, kExt2FeatureIncompatFiletype);
    const char volume[] = "obd-convert";
    std::memcpy(super.data() + 120, volume, sizeof(volume) - 1);
    full_pwrite(fd, super.data(), super.size(), 1024);

    std::vector<uint8_t> group(kBlockSize, 0);
    obd::bytes::store_u32_le(group.data() + 0, 2);
    obd::bytes::store_u32_le(group.data() + 4, 3);
    obd::bytes::store_u32_le(group.data() + 8, inode_table_block);
    obd::bytes::store_u16_le(group.data() + 12, static_cast<uint16_t>(free_blocks));
    obd::bytes::store_u16_le(group.data() + 14, static_cast<uint16_t>(free_inodes));
    obd::bytes::store_u16_le(group.data() + 16, static_cast<uint16_t>(count_dirs(root)));
    full_pwrite(fd, group.data(), group.size(), kBlockSize);

    std::vector<uint8_t> block_bitmap(kBlockSize, 0);
    for (uint32_t i = 0; i < used_blocks; ++i) set_bitmap_bit(block_bitmap, i);
    for (uint32_t i = static_cast<uint32_t>(total_blocks); i < kMaxBlocks; ++i) {
        set_bitmap_bit(block_bitmap, i);
    }
    full_pwrite(fd, block_bitmap.data(), block_bitmap.size(), 2ull * kBlockSize);

    std::vector<uint8_t> inode_bitmap(kBlockSize, 0);
    for (uint32_t i = 0; i < 10; ++i) set_bitmap_bit(inode_bitmap, i);
    auto mark_inode = [&](auto&& self, const Node& node) -> void {
        set_bitmap_bit(inode_bitmap, node.inode - 1);
        for (const auto& [_, child] : node.children) self(self, *child);
    };
    mark_inode(mark_inode, root);
    for (uint32_t i = inode_count; i < kBlockSize * 8; ++i) set_bitmap_bit(inode_bitmap, i);
    full_pwrite(fd, inode_bitmap.data(), inode_bitmap.size(), 3ull * kBlockSize);

    write_node_inode(fd, root, inode_table_block);
    write_file_payload(fd, root);
    if (::fsync(fd) != 0) obd::throw_errno(errno, "fsync failed for raw filesystem");
}

#if OBD_HAVE_LIBE2FS

class Ext2FsGuard {
public:
    explicit Ext2FsGuard(ext2_filsys fs = nullptr) : fs_(fs) {}
    Ext2FsGuard(const Ext2FsGuard&) = delete;
    Ext2FsGuard& operator=(const Ext2FsGuard&) = delete;
    ~Ext2FsGuard() {
        if (fs_ != nullptr) ext2fs_close_free(&fs_);
    }
    ext2_filsys get() const { return fs_; }
    ext2_filsys release() {
        ext2_filsys out = fs_;
        fs_ = nullptr;
        return out;
    }
    void reset(ext2_filsys fs = nullptr) {
        if (fs_ != nullptr) ext2fs_close_free(&fs_);
        fs_ = fs;
    }

private:
    ext2_filsys fs_ = nullptr;
};

class Ext2FileGuard {
public:
    explicit Ext2FileGuard(ext2_file_t file = nullptr) : file_(file) {}
    Ext2FileGuard(const Ext2FileGuard&) = delete;
    Ext2FileGuard& operator=(const Ext2FileGuard&) = delete;
    ~Ext2FileGuard() {
        if (file_ != nullptr) ext2fs_file_close(file_);
    }
    ext2_file_t get() const { return file_; }
    errcode_t close() {
        if (file_ == nullptr) return 0;
        ext2_file_t file = file_;
        file_ = nullptr;
        return ext2fs_file_close(file);
    }
    void reset(ext2_file_t file = nullptr) {
        if (file_ != nullptr) ext2fs_file_close(file_);
        file_ = file;
    }

private:
    ext2_file_t file_ = nullptr;
};

[[noreturn]] void throw_libe2fs_error(errcode_t err, const std::string& what) {
    throw std::runtime_error(what + ": " + error_message(err));
}

void check_libe2fs(errcode_t err, const std::string& what) {
    if (err != 0) throw_libe2fs_error(err, what);
}

void pin_libe2fs_superblock_fields(struct ext2_super_block& super) {
    static constexpr __u8 kUuid[16] = {
        0x4f, 0x42, 0x44, 0x45, 0x4c, 0x49, 0x4f, 0x2d,
        0x43, 0x4f, 0x4e, 0x56, 0x45, 0x52, 0x54, 0x00,
    };
    std::memcpy(super.s_uuid, kUuid, sizeof(kUuid));
    std::memset(super.s_volume_name, 0, sizeof(super.s_volume_name));
    const char volume[] = "obd-convert";
    std::memcpy(super.s_volume_name, volume,
                std::min(sizeof(super.s_volume_name), sizeof(volume) - 1));
    super.s_mtime = 0;
    super.s_wtime = 0;
    super.s_lastcheck = 0;
    super.s_mkfs_time = 0;
    super.s_mtime_hi = 0;
    super.s_wtime_hi = 0;
    super.s_lastcheck_hi = 0;
    super.s_mkfs_time_hi = 0;
}

void pin_libe2fs_superblock(ext2_filsys fs) {
    pin_libe2fs_superblock_fields(*fs->super);
}

bool ext2_group_has_super(const struct ext2_super_block& super, uint64_t group) {
    if (group == 0) return true;
    if ((super.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0) return true;
    if (group == 1) return true;
    if ((group & 1u) == 0) return false;
    auto test_root = [](uint64_t value, uint64_t root) {
        while (value > root && value % root == 0) value /= root;
        return value == root;
    };
    return test_root(group, 3) || test_root(group, 5) || test_root(group, 7);
}

void pin_libe2fs_superblock_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) obd::throw_errno(errno, "cannot reopen raw filesystem " + path);
    FdGuard guard(fd);
    struct ext2_super_block primary {};
    full_pread(fd, &primary, sizeof(primary), 1024);
    const uint64_t block_size = 1024ull << primary.s_log_block_size;
    const uint64_t total_blocks = primary.s_blocks_count;
    const uint64_t first_data_block = primary.s_first_data_block;
    const uint64_t blocks_per_group = primary.s_blocks_per_group;
    if (block_size == 0 || blocks_per_group == 0 || total_blocks < first_data_block) {
        throw std::runtime_error("invalid libe2fs superblock geometry");
    }
    const uint64_t groups = div_ceil(total_blocks - first_data_block, blocks_per_group);
    for (uint64_t group = 0; group < groups; ++group) {
        if (!ext2_group_has_super(primary, group)) continue;
        const uint64_t offset = group == 0 ? 1024 :
            (first_data_block + group * blocks_per_group) * block_size;
        struct ext2_super_block super {};
        full_pread(fd, &super, sizeof(super), offset);
        pin_libe2fs_superblock_fields(super);
        full_pwrite(fd, &super, sizeof(super), offset);
    }
    if (::fsync(fd) != 0) obd::throw_errno(errno, "fsync failed for raw filesystem");
}

void set_inode_owner(struct ext2_inode& inode, uint32_t uid, uint32_t gid) {
    inode.i_uid = static_cast<__u16>(uid & 0xffffu);
    ext2fs_set_i_uid_high(inode, static_cast<__u16>(uid >> 16));
    inode.i_gid = static_cast<__u16>(gid & 0xffffu);
    ext2fs_set_i_gid_high(inode, static_cast<__u16>(gid >> 16));
}

void set_inode_common(ext2_filsys fs, ext2_ino_t ino, uint16_t type,
                      uint16_t perm, uint32_t uid, uint32_t gid) {
    struct ext2_inode inode {};
    check_libe2fs(ext2fs_read_inode(fs, ino, &inode), "libe2fs read inode");
    inode.i_mode = static_cast<__u16>(type | perm);
    set_inode_owner(inode, uid, gid);
    inode.i_atime = 0;
    inode.i_ctime = 0;
    inode.i_mtime = 0;
    check_libe2fs(ext2fs_write_inode(fs, ino, &inode), "libe2fs write inode");
}

void link_with_expand(ext2_filsys fs, ext2_ino_t parent, const std::string& name,
                      ext2_ino_t ino, int file_type) {
    errcode_t err = ext2fs_link(fs, parent, name.c_str(), ino, file_type);
    if (err == EXT2_ET_DIR_NO_SPACE) {
        check_libe2fs(ext2fs_expand_dir(fs, parent), "libe2fs expand directory");
        err = ext2fs_link(fs, parent, name.c_str(), ino, file_type);
    }
    check_libe2fs(err, "libe2fs link " + name);
}

ext2_ino_t lookup_child(ext2_filsys fs, ext2_ino_t parent, const std::string& name) {
    ext2_ino_t ino = 0;
    check_libe2fs(ext2fs_lookup(fs, parent, name.c_str(), static_cast<int>(name.size()),
                                nullptr, &ino),
                  "libe2fs lookup " + name);
    return ino;
}

void write_libe2fs_file_payload(ext2_filsys fs, ext2_ino_t ino, const Node& node) {
    if (node.size == 0) return;
    ext2_file_t raw_file = nullptr;
    check_libe2fs(ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &raw_file),
                  "libe2fs open file " + node.name);
    Ext2FileGuard file(raw_file);

    const int in = ::open(node.spool_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) obd::throw_errno(errno, "cannot open payload spool " + node.spool_path);
    FdGuard guard(in);
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t left = node.size; left > 0;) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buf.size(), left));
        read_exact(in, buf.data(), chunk, false);
        const uint8_t* p = buf.data();
        size_t remaining = chunk;
        while (remaining > 0) {
            unsigned int written = 0;
            check_libe2fs(ext2fs_file_write(file.get(), p,
                                            static_cast<unsigned int>(remaining),
                                            &written),
                          "libe2fs write file " + node.name);
            if (written == 0) throw std::runtime_error("libe2fs short file write");
            p += written;
            remaining -= written;
        }
        left -= chunk;
    }
    check_libe2fs(file.close(), "libe2fs close file " + node.name);
}

void reserve_libe2fs_fixed_inodes(ext2_filsys fs) {
    const ext2_ino_t first_dynamic = EXT2_FIRST_INODE(fs->super);
    for (ext2_ino_t ino = 1; ino < first_dynamic; ++ino) {
        if (ino == EXT2_ROOT_INO) continue;
        ext2fs_inode_alloc_stats2(fs, ino, +1, 0);
    }
}

void write_libe2fs_node(ext2_filsys fs, ext2_ino_t parent, const Node& node) {
    if (node.kind == NodeKind::Dir) {
        ext2_ino_t ino = 0;
        errcode_t err = ext2fs_mkdir(fs, parent, 0, node.name.c_str());
        if (err == EXT2_ET_DIR_NO_SPACE) {
            check_libe2fs(ext2fs_expand_dir(fs, parent), "libe2fs expand directory");
            err = ext2fs_mkdir(fs, parent, 0, node.name.c_str());
        }
        check_libe2fs(err, "libe2fs mkdir " + node.name);
        ino = lookup_child(fs, parent, node.name);
        for (const auto& [_, child] : node.children) write_libe2fs_node(fs, ino, *child);
        set_inode_common(fs, ino, LINUX_S_IFDIR, node.perm, node.uid, node.gid);
        return;
    }

    if (node.kind == NodeKind::Symlink) {
        errcode_t err = ext2fs_symlink(fs, parent, 0, node.name.c_str(),
                                       node.symlink_target.c_str());
        if (err == EXT2_ET_DIR_NO_SPACE) {
            check_libe2fs(ext2fs_expand_dir(fs, parent), "libe2fs expand directory");
            err = ext2fs_symlink(fs, parent, 0, node.name.c_str(),
                                 node.symlink_target.c_str());
        }
        check_libe2fs(err, "libe2fs symlink " + node.name);
        const ext2_ino_t ino = lookup_child(fs, parent, node.name);
        set_inode_common(fs, ino, LINUX_S_IFLNK, node.perm, node.uid, node.gid);
        return;
    }

    ext2_ino_t ino = 0;
    check_libe2fs(ext2fs_new_inode(fs, parent, LINUX_S_IFREG | node.perm, nullptr, &ino),
                  "libe2fs allocate inode " + node.name);
    link_with_expand(fs, parent, node.name, ino, EXT2_FT_REG_FILE);
    ext2fs_inode_alloc_stats2(fs, ino, +1, 0);

    struct ext2_inode inode {};
    inode.i_mode = static_cast<__u16>(LINUX_S_IFREG | node.perm);
    inode.i_links_count = 1;
    set_inode_owner(inode, node.uid, node.gid);
    inode.i_atime = 0;
    inode.i_ctime = 0;
    inode.i_mtime = 0;
    check_libe2fs(ext2fs_inode_size_set(fs, &inode, node.size),
                  "libe2fs set file size " + node.name);
    check_libe2fs(ext2fs_write_new_inode(fs, ino, &inode),
                  "libe2fs write file inode " + node.name);
    write_libe2fs_file_payload(fs, ino, node);
    set_inode_common(fs, ino, LINUX_S_IFREG, node.perm, node.uid, node.gid);
}

void write_libe2fs_image(Node& root, const std::string& path, uint64_t requested_size) {
    const uint64_t raw_size = requested_size == 0 ? auto_libe2fs_size_bytes(root) : requested_size;
    validate_explicit_image_blocks(raw_size, ConverterBackend::LibE2fs);
    const uint64_t total_blocks = raw_size / kBlockSize;
    if (total_blocks > std::numeric_limits<__u32>::max()) {
        throw std::runtime_error("libe2fs backend currently supports images up to 2^32-1 blocks");
    }
    const uint32_t nodes = count_nodes(root);
    const uint32_t inode_count = static_cast<uint32_t>(round_up(
        std::max<uint32_t>(nodes + 32, kMinBuiltInInodes), 128));

    const int fd =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) obd::throw_errno(errno, "cannot create raw filesystem " + path);
    FdGuard file(fd);
    if (::ftruncate(fd, static_cast<off_t>(raw_size)) != 0) {
        obd::throw_errno(errno, "cannot size raw filesystem " + path);
    }
    file.reset();

    struct ext2_super_block param {};
    param.s_rev_level = EXT2_DYNAMIC_REV;
    param.s_log_block_size = 2;
    param.s_log_cluster_size = 2;
    param.s_blocks_count = static_cast<__u32>(total_blocks);
    param.s_inodes_count = inode_count;
    param.s_inode_size = EXT2_GOOD_OLD_INODE_SIZE;
    param.s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
    param.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
    param.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
    param.s_errors = EXT2_ERRORS_DEFAULT;
    pin_libe2fs_superblock_fields(param);

    if (::setenv("E2FSPROGS_FAKE_TIME", "1", 1) != 0) {
        obd::throw_errno(errno, "cannot set deterministic libe2fs time");
    }

    ext2_filsys raw_fs = nullptr;
    check_libe2fs(ext2fs_initialize(path.c_str(), EXT2_FLAG_RW | EXT2_FLAG_EXCLUSIVE,
                                    &param, unix_io_manager, &raw_fs),
                  "libe2fs initialize filesystem");
    Ext2FsGuard fs(raw_fs);
    fs.get()->now = 1;
    pin_libe2fs_superblock(fs.get());
    check_libe2fs(ext2fs_allocate_tables(fs.get()), "libe2fs allocate tables");
    reserve_libe2fs_fixed_inodes(fs.get());
    check_libe2fs(ext2fs_mkdir(fs.get(), EXT2_ROOT_INO, EXT2_ROOT_INO, nullptr),
                  "libe2fs create root directory");
    for (const auto& [_, child] : root.children) write_libe2fs_node(fs.get(), EXT2_ROOT_INO, *child);
    set_inode_common(fs.get(), EXT2_ROOT_INO, LINUX_S_IFDIR, root.perm, root.uid, root.gid);
    check_libe2fs(ext2fs_write_bitmaps(fs.get()), "libe2fs write bitmaps");
    ext2_filsys closing = fs.release();
    check_libe2fs(ext2fs_close(closing), "libe2fs close filesystem");
    pin_libe2fs_superblock_file(path);
}

#else

void write_libe2fs_image(Node&, const std::string&, uint64_t) {
    throw std::runtime_error("libe2fs backend was not enabled at build time");
}

#endif

struct Options {
    std::vector<std::string> inputs;
    std::string import_package;
    std::string descriptor;
    std::string parent_config;
    uint64_t import_metadata_budget = obd::convert::kDefaultTurboMetadataBudget;
    bool import_budget_set = false;
    std::string input;
    std::string out_dir;
    std::string name = "layer";
#if OBD_HAVE_LIBE2FS
    ConverterBackend backend = ConverterBackend::LibE2fs;
#else
    ConverterBackend backend = ConverterBackend::BuiltinExt2;
#endif
    uint64_t size = 0;
    bool keep_raw = false;
    bool turbo_oci = false;
};

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (++i >= argc) throw UsageError(std::string("missing value for ") + what);
            return argv[i];
        };
        if (a == "--input") {
            opts.input = next("--input");
            opts.inputs.push_back(opts.input);
        }
        else if (a == "--import-turboOCI") opts.import_package = next("--import-turboOCI");
        else if (a == "--max-import-metadata-size") {
            opts.import_metadata_budget = parse_size_arg(next("--max-import-metadata-size"), "--max-import-metadata-size");
            opts.import_budget_set = true;
        }
        else if (a == "--parent-config") opts.parent_config = next("--parent-config");
        else if (a == "--descriptor") opts.descriptor = next("--descriptor");
        else if (a == "--out-dir") opts.out_dir = next("--out-dir");
        else if (a == "--name") opts.name = next("--name");
        else if (a == "--backend") {
            const std::string value = next("--backend");
            if (value == "builtin-ext2") opts.backend = ConverterBackend::BuiltinExt2;
            else if (value == "libe2fs") opts.backend = ConverterBackend::LibE2fs;
            else throw UsageError("--backend must be builtin-ext2 or libe2fs");
        }
        else if (a == "--size") opts.size = parse_size_arg(next("--size"), "--size");
        else if (a == "--keep-raw") opts.keep_raw = true;
        else if (a == "--turboOCI") opts.turbo_oci = true;
        else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw UsageError("unknown argument: " + a);
        }
    }
    if (opts.input.empty() || opts.out_dir.empty()) {
        usage(argv[0]);
        std::exit(2);
    }
    validate_output_name(opts.name);
    if (opts.inputs.size() > 1 && (!opts.turbo_oci || !opts.import_package.empty()))
        throw UsageError("repeated --input requires --turboOCI conversion");
    if (!opts.import_package.empty()) {
        if (opts.descriptor.empty() || opts.input == "-" || opts.turbo_oci || opts.size != 0 || opts.keep_raw)
            throw UsageError("--import-turboOCI requires --descriptor and a local --input; conversion options cannot be combined");
    } else if (!opts.descriptor.empty() || !opts.parent_config.empty() || opts.import_budget_set) {
        throw UsageError("--descriptor, --parent-config and --max-import-metadata-size require --import-turboOCI");
    }
#if !OBD_HAVE_LIBE2FS
    if (opts.backend == ConverterBackend::LibE2fs) {
        throw UsageError("--backend libe2fs requires a build with OBD_ENABLE_LIBE2FS_BACKEND=ON");
    }
#endif
    if (opts.turbo_oci && (opts.backend != ConverterBackend::LibE2fs || opts.input == "-"))
        throw UsageError("--turboOCI requires libe2fs and a local seekable tar or gzip input file");
    std::filesystem::create_directories(opts.out_dir);
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options opts = parse_args(argc, argv);
        if (!opts.import_package.empty()) {
            const auto imported = obd::convert::import_turbo_image(
                opts.import_package, opts.descriptor, opts.input, opts.out_dir + "/" + opts.name, opts.parent_config, opts.import_metadata_budget);
            std::puts(imported.dump(2).c_str());
            return 0;
        }
        if (opts.turbo_oci) {
            auto converted = obd::convert::convert_turbo_layers(
                opts.inputs, opts.out_dir + "/" + opts.name, opts.size, opts.keep_raw);
            std::puts(converted.dump(2).c_str());
            return 0;
        }
        FdGuard input;
        if (opts.input == "-") {
            input.fd = STDIN_FILENO;
        } else {
            const int fd = ::open(opts.input.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) obd::throw_errno(errno, "cannot open " + opts.input);
            input.fd = fd;
        }

        Node root("", NodeKind::Dir, nullptr);
        TempWorkspace workspace(opts.out_dir, opts.name);
        const uint64_t image_block_budget = image_block_budget_for(opts.size, opts.backend);
        TarReader reader{input.fd, workspace.path(), opts.name,
                         opts.backend, image_block_budget};
        reader.load_into(root);
        if (opts.input == "-") input.release();

        const std::string raw_path = workspace.path() + "/rootfs.ext2";
        const std::string lsmt_tmp_path = workspace.path() + "/layer.lsmt";
        const std::string lsmt_path = opts.out_dir + "/" + opts.name + ".lsmt";
        const std::string keep_raw_path = opts.out_dir + "/." + opts.name + ".ext2.tmp";
        if (opts.backend == ConverterBackend::LibE2fs) {
            write_libe2fs_image(root, raw_path, opts.size);
        } else {
            write_ext2_image(root, raw_path, opts.size);
        }

        const int raw_fd = ::open(raw_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (raw_fd < 0) obd::throw_errno(errno, "cannot open raw filesystem " + raw_path);
        FdGuard raw(raw_fd);
        const uint64_t raw_size = file_size(raw_path);
        const std::string raw_digest = sha256_of_fd(raw_fd, raw_size);
        obd::format::LsmtWriteOptions lopts;
        lopts.uuid = uuid_from_digest(raw_digest);
        lopts.user_tag = std::string("obd-convert ") + std::string(backend_name(opts.backend));
        obd::format::write_lsmt_single_layer(raw_fd, raw_size, lsmt_tmp_path, lopts);
        raw.reset();
        fsync_file(lsmt_tmp_path, "LSMT layer");
        const std::string lsmt_digest = sha256_of_file(lsmt_tmp_path);
        const uint64_t lsmt_size = file_size(lsmt_tmp_path);
        if (opts.keep_raw) {
            fsync_file(raw_path, "raw filesystem");
            replace_file_atomically(raw_path, keep_raw_path);
        }
        replace_file_atomically(lsmt_tmp_path, lsmt_path);
        fsync_directory(opts.out_dir);

        nlohmann::json lower;
        lower["digest"] = "sha256:" + lsmt_digest;
        lower["size"] = lsmt_size;
        lower["file"] = lsmt_path;
        nlohmann::json snippet;
        snippet["repoBlobUrl"] = "";
        snippet["lowers"] = nlohmann::json::array({lower});
        snippet["converter"] = {
            {"backend", std::string(backend_name(opts.backend))},
            {"filesystem", "ext2"},
            {"raw_digest", "sha256:" + raw_digest},
            {"virtual_size", raw_size},
        };
        std::puts(snippet.dump(2).c_str());
        return 0;
    } catch (const UsageError& e) {
        std::fprintf(stderr, "obd-convert: %s\n", e.what());
        return 2;
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "obd-convert: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "obd-convert: %s\n", e.what());
        return 1;
    }
}
