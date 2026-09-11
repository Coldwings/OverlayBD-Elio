// obd-convert: build a deterministic filesystem layer from a tar stream.
// The built-in backend writes a bounded ext2 image directly (no mount, no
// device, no mkfs subprocess), then seals it through the LSMT writer.
#include "common/bytes.hpp"
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include "format/writer.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

constexpr uint16_t kExt2SIfReg = 0100000;
constexpr uint16_t kExt2SIfDir = 0040000;
constexpr uint16_t kExt2SIfLnk = 0120000;
constexpr uint32_t kExt2FeatureIncompatFiletype = 0x0002;

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
                 "          [--size bytes] [--keep-raw]\n"
                 "\n"
                 "Builds <out-dir>/<name>.lsmt from a ustar rootfs stream using\n"
                 "the deterministic built-in ext2 backend. No device, mount, or\n"
                 "mkfs subprocess is used. JSON manifest metadata is printed to\n"
                 "stdout. The built-in backend supports regular files,\n"
                 "directories, and short symlinks.\n",
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
        : name(std::move(n)), kind(k), parent(p) {}

    std::string name;
    NodeKind kind;
    Node* parent = nullptr;
    std::map<std::string, std::unique_ptr<Node>> children;
    uint16_t perm = 0755;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t size = 0;
    std::string spool_path;
    std::string symlink_target;
    uint32_t inode = 0;
    std::vector<uint32_t> blocks;
    uint32_t indirect_block = 0;
    std::vector<uint8_t> dir_data;
};

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

void throw_too_many_inodes() {
    throw std::runtime_error("built-in ext2 backend supports at most 32768 inodes");
}

Node& ensure_dir(Node& root, const std::vector<std::string>& parts,
                 uint32_t* node_count = nullptr) {
    Node* cur = &root;
    for (const auto& part : parts) {
        auto it = cur->children.find(part);
        if (it == cur->children.end()) {
            if (node_count != nullptr) {
                if (*node_count >= kMaxBuiltInNodes) throw_too_many_inodes();
                ++*node_count;
            }
            auto dir = std::make_unique<Node>(part, NodeKind::Dir, cur);
            it = cur->children.emplace(part, std::move(dir)).first;
        }
        if (it->second->kind != NodeKind::Dir) {
            throw std::runtime_error("tar path parent is not a directory: " + part);
        }
        cur = it->second.get();
    }
    return *cur;
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

struct TarReader {
    int fd;
    std::string work_dir;
    std::string stem;
    std::vector<std::string> spool_paths;
    uint64_t next_spool = 0;
    uint32_t node_count = 1;

    TarReader(int input_fd, std::string workspace_dir, std::string output_stem)
        : fd(input_fd), work_dir(std::move(workspace_dir)), stem(std::move(output_stem)) {}

    ~TarReader() {
        for (const auto& path : spool_paths) ::unlink(path.c_str());
    }

    std::string make_spool_path() {
        return work_dir + "/" + stem + ".entry-" + std::to_string(next_spool++) + ".tmp";
    }

    void skip_padding(uint64_t payload_size) {
        const uint64_t pad = (512 - (payload_size % 512)) % 512;
        std::array<uint8_t, 512> discard {};
        if (pad != 0) read_exact(fd, discard.data(), static_cast<size_t>(pad), false);
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
            read_exact(fd, buf.data(), chunk, false);
            full_write(out, buf.data(), chunk);
            left -= chunk;
        }
        if (::fsync(out) != 0) obd::throw_errno(errno, "fsync failed for payload spool");
        return path;
    }

    void load_into(Node& root) {
        for (;;) {
            std::array<uint8_t, 512> header {};
            if (!read_exact(fd, header.data(), header.size(), true)) break;
            if (all_zero(header)) break;
            verify_tar_checksum(header);

            std::string name = tar_string(header.data(), 100);
            const std::string prefix = tar_string(header.data() + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
            const char typeflag = header[156] == '\0' ? '0' : static_cast<char>(header[156]);
            const uint64_t mode = parse_octal_field(header.data() + 100, 8, "mode");
            const uint64_t uid = parse_octal_field(header.data() + 108, 8, "uid");
            const uint64_t gid = parse_octal_field(header.data() + 116, 8, "gid");
            const uint64_t size = parse_octal_field(header.data() + 124, 12, "size");
            if (uid > 65535 || gid > 65535) {
                throw std::runtime_error("built-in ext2 backend supports uid/gid <= 65535");
            }
            const uint16_t perm = static_cast<uint16_t>(mode & 07777);

            if (typeflag == '5') {
                if (size != 0) throw std::runtime_error("directory tar entry has a payload");
                auto parts = split_tar_path(name, NodeKind::Dir);
                Node& dir = ensure_dir(root, parts, &node_count);
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
            Node& parent = ensure_dir(root, parent_parts, &node_count);
            const std::string leaf = parts.back();
            if (parent.children.count(leaf) != 0) {
                throw std::runtime_error("duplicate tar entry: " + name);
            }
            if (node_count >= kMaxBuiltInNodes) throw_too_many_inodes();
            ++node_count;
            auto node = std::make_unique<Node>(leaf, kind, &parent);
            node->perm = perm;
            node->uid = static_cast<uint32_t>(uid);
            node->gid = static_cast<uint32_t>(gid);
            if (kind == NodeKind::File) {
                if (size > kMaxBuiltInFileBytes) {
                    throw std::runtime_error("built-in ext2 backend file is too large: " + name);
                }
                node->size = size;
                if (size != 0) node->spool_path = spool_payload(size);
                skip_padding(size);
            } else {
                if (size != 0) throw std::runtime_error("symlink tar entry has a payload");
                node->symlink_target = tar_string(header.data() + 157, 100);
                if (node->symlink_target.empty()) {
                    throw std::runtime_error("symlink tar entry has an empty target");
                }
                if (node->symlink_target.size() > 60) {
                    throw std::runtime_error("built-in ext2 backend supports symlink targets <= 60 bytes");
                }
                node->size = node->symlink_target.size();
            }
            parent.children.emplace(leaf, std::move(node));
        }
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

size_t min_dirent_len(size_t name_len) { return static_cast<size_t>(round_up(8 + name_len, 4)); }

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

void write_ext2_image(Node& root, const std::string& path, uint64_t requested_size) {
    assign_inodes(root);
    build_directory_payloads(root);

    const uint32_t nodes = count_nodes(root);
    const uint32_t used_inodes = 10 + (nodes - 1);
    if (used_inodes > kMaxBuiltInInodes) {
        throw_too_many_inodes();
    }
    uint32_t desired_inodes = std::max<uint32_t>(used_inodes + 32, 128);
    desired_inodes = std::min<uint32_t>(desired_inodes, kMaxBuiltInInodes);
    const uint32_t inode_count =
        static_cast<uint32_t>(round_up(desired_inodes, 128));
    const uint32_t inode_table_blocks = static_cast<uint32_t>(div_ceil(
        static_cast<uint64_t>(inode_count) * kInodeSize, kBlockSize));
    const uint32_t inode_table_block = 4;
    const uint32_t first_data_block = inode_table_block + inode_table_blocks;
    const uint64_t payload_blocks = count_payload_blocks(root);
    const uint64_t required_blocks = first_data_block + payload_blocks;

    uint64_t total_blocks = 0;
    if (requested_size != 0) {
        if (requested_size % kBlockSize != 0) {
            throw std::runtime_error("--size must be a multiple of 4096 for the built-in ext2 backend");
        }
        total_blocks = requested_size / kBlockSize;
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

struct Options {
    std::string input;
    std::string out_dir;
    std::string name = "layer";
    uint64_t size = 0;
    bool keep_raw = false;
};

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (++i >= argc) throw UsageError(std::string("missing value for ") + what);
            return argv[i];
        };
        if (a == "--input") opts.input = next("--input");
        else if (a == "--out-dir") opts.out_dir = next("--out-dir");
        else if (a == "--name") opts.name = next("--name");
        else if (a == "--size") opts.size = parse_size_arg(next("--size"), "--size");
        else if (a == "--keep-raw") opts.keep_raw = true;
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
    std::filesystem::create_directories(opts.out_dir);
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options opts = parse_args(argc, argv);
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
        TarReader reader{input.fd, workspace.path(), opts.name};
        reader.load_into(root);
        if (opts.input == "-") input.release();

        const std::string raw_path = workspace.path() + "/rootfs.ext2";
        const std::string lsmt_tmp_path = workspace.path() + "/layer.lsmt";
        const std::string lsmt_path = opts.out_dir + "/" + opts.name + ".lsmt";
        const std::string keep_raw_path = opts.out_dir + "/." + opts.name + ".ext2.tmp";
        write_ext2_image(root, raw_path, opts.size);

        const int raw_fd = ::open(raw_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (raw_fd < 0) obd::throw_errno(errno, "cannot open raw filesystem " + raw_path);
        FdGuard raw(raw_fd);
        const uint64_t raw_size = file_size(raw_path);
        const std::string raw_digest = sha256_of_fd(raw_fd, raw_size);
        obd::format::LsmtWriteOptions lopts;
        lopts.uuid = uuid_from_digest(raw_digest);
        lopts.user_tag = "obd-convert builtin-ext2";
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
            {"backend", "builtin-ext2"},
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
