// Unit tests: obd-convert CLI — rootfs tar stream to deterministic LSMT layer.
#include "common/bytes.hpp"
#include "common/sha256.hpp"
#include "format/lsmt.hpp"
#include "source/local_file.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace obd;
using obd::test::TempDir;

#ifndef OBD_TEST_OBD_CONVERT_BIN
#define OBD_TEST_OBD_CONVERT_BIN "obd-convert"
#endif

namespace {

constexpr size_t kExt2BlockSize = 4096;
constexpr size_t kIndirectToolSize = 13 * kExt2BlockSize + 123;
constexpr size_t kExt2PointersPerBlock = kExt2BlockSize / 4;
constexpr uint32_t kExt2FeatureRoCompatLargeFile = 0x0002;
constexpr uint64_t kMaxBuiltInFileBytes =
    static_cast<uint64_t>(12 + kExt2PointersPerBlock) * kExt2BlockSize;

struct CommandResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

void write_all_fd(int fd, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t w = ::write(fd, p + done, size - done);
        REQUIRE(w > 0);
        done += static_cast<size_t>(w);
    }
}

std::string read_all_fd(int fd) {
    std::string out;
    std::array<char, 4096> buf {};
    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r < 0 && errno == EINTR) continue;
        REQUIRE(r >= 0);
        if (r == 0) break;
        out.append(buf.data(), static_cast<size_t>(r));
    }
    return out;
}

CommandResult run_convert(const std::vector<std::string>& args,
                          const std::vector<uint8_t>* stdin_data = nullptr,
                          bool force_builtin_backend = true) {
    int stdout_pipe[2];
    int stderr_pipe[2];
    REQUIRE(::pipe(stdout_pipe) == 0);
    REQUIRE(::pipe(stderr_pipe) == 0);
    int stdin_pipe[2] = {-1, -1};
    if (stdin_data) REQUIRE(::pipe(stdin_pipe) == 0);

    std::vector<std::string> storage;
    storage.emplace_back(OBD_TEST_OBD_CONVERT_BIN);
    const bool has_backend = std::find(args.begin(), args.end(), "--backend") != args.end();
    if (force_builtin_backend && !has_backend) {
        storage.emplace_back("--backend");
        storage.emplace_back("builtin-ext2");
    }
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        if (stdin_data) {
            ::close(stdin_pipe[1]);
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::close(stdin_pipe[0]);
        }
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execv(OBD_TEST_OBD_CONVERT_BIN, argv.data());
        _exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    if (stdin_data) {
        ::close(stdin_pipe[0]);
        write_all_fd(stdin_pipe[1], stdin_data->data(), stdin_data->size());
        ::close(stdin_pipe[1]);
    }

    CommandResult result;
    result.out = read_all_fd(stdout_pipe[0]);
    result.err = read_all_fd(stderr_pipe[0]);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    result.exit_code = WEXITSTATUS(status);
    return result;
}

void fill_octal(uint8_t* out, size_t width, uint64_t value) {
    std::memset(out, 0, width);
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%0%zulo", width - 1);
    std::snprintf(reinterpret_cast<char*>(out), width, fmt,
                  static_cast<unsigned long>(value));
}

std::array<uint8_t, 512> make_tar_header(const std::string& name, char typeflag,
                                          uint64_t mode, uint64_t uid,
                                          uint64_t gid, uint64_t payload_size,
                                          const std::string& link_target = {}) {
    std::array<uint8_t, 512> h {};
    REQUIRE(name.size() <= 100);
    std::memcpy(h.data(), name.data(), name.size());
    fill_octal(h.data() + 100, 8, mode);
    fill_octal(h.data() + 108, 8, uid);
    fill_octal(h.data() + 116, 8, gid);
    fill_octal(h.data() + 124, 12, payload_size);
    fill_octal(h.data() + 136, 12, 0);
    h[156] = static_cast<uint8_t>(typeflag);
    if (!link_target.empty()) {
        REQUIRE(link_target.size() <= 100);
        std::memcpy(h.data() + 157, link_target.data(), link_target.size());
    }
    std::memcpy(h.data() + 257, "ustar", 5);
    std::memcpy(h.data() + 263, "00", 2);
    std::memset(h.data() + 148, ' ', 8);
    uint32_t sum = 0;
    for (uint8_t b : h) sum += b;
    std::snprintf(reinterpret_cast<char*>(h.data() + 148), 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
    return h;
}

void append_tar_entry(std::vector<uint8_t>& tar, const std::string& name,
                      char typeflag, uint64_t mode, uint64_t uid,
                      uint64_t gid, const std::vector<uint8_t>& payload = {},
                      const std::string& link_target = {}) {
    const auto h = make_tar_header(name, typeflag, mode, uid, gid,
                                   payload.size(), link_target);
    tar.insert(tar.end(), h.begin(), h.end());
    tar.insert(tar.end(), payload.begin(), payload.end());
    tar.resize(static_cast<size_t>(((tar.size() + 511) / 512) * 512), 0);
}

void rewrite_tar_checksum(std::vector<uint8_t>& tar, size_t header_offset) {
    REQUIRE(header_offset + 512 <= tar.size());
    uint8_t* h = tar.data() + header_offset;
    std::memset(h + 148, ' ', 8);
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += h[i];
    std::snprintf(reinterpret_cast<char*>(h + 148), 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
}

std::vector<uint8_t> make_rootfs_tar() {
    std::vector<uint8_t> tar;
    append_tar_entry(tar, "etc/", '5', 0750, 5, 6);
    const std::vector<uint8_t> hello = {'h', 'e', 'l', 'l', 'o', '\n'};
    append_tar_entry(tar, "etc/hello.txt", '0', 0640, 1000, 1001, hello);
    const std::vector<uint8_t> tool = test::pattern_bytes(kIndirectToolSize, 77);
    append_tar_entry(tar, "bin/tool", '0', 0755, 0, 0, tool);
    append_tar_entry(tar, "link-to-hello", '2', 0777, 7, 8, {}, "etc/hello.txt");
    append_tar_entry(tar, "zero-dir/", '5', 0000, 9, 10);
    append_tar_entry(tar, "zero-file", '0', 0000, 11, 12,
                     std::vector<uint8_t>{'z'});
    append_tar_entry(tar, "zero-link", '2', 0000, 13, 14, {}, "etc/hello.txt");
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

std::vector<uint8_t> make_tiny_file_tar() {
    std::vector<uint8_t> tar;
    append_tar_entry(tar, "tiny", '0', 0644, 0, 0, {'x'});
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

std::vector<uint8_t> make_file_then_directory_budget_tar() {
    std::vector<uint8_t> tar;
    append_tar_entry(tar, "tiny", '0', 0644, 0, 0, {'x'});
    append_tar_entry(tar, "extra/", '5', 0755, 0, 0);
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

std::vector<uint8_t> make_many_root_files_tar(size_t count) {
    std::vector<uint8_t> tar;
    tar.reserve((count + 2) * 512);
    for (size_t i = 0; i < count; ++i) {
        append_tar_entry(tar, "f" + std::to_string(i), '0', 0644, 0, 0);
    }
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

[[maybe_unused]] std::vector<uint8_t> make_libe2fs_feature_tar() {
    std::vector<uint8_t> tar;
    append_tar_entry(tar, "wide-owner", '0', 0644, 70000, 80000, {'w'});
    append_tar_entry(tar, "slow-link", '2', 0777, 70001, 80001, {}, std::string(60, 't'));
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

std::vector<uint8_t> make_sorted_directory_budget_tar() {
    std::vector<uint8_t> tar;
    constexpr size_t kExistingEntries = 156;
    tar.reserve((kExistingEntries + 3) * 512);
    for (size_t i = 0; i < kExistingEntries; ++i) {
        std::string name = "b" + std::to_string(100 + i) + std::string(37, 'x');
        REQUIRE(name.size() == 41);
        append_tar_entry(tar, name, '2', 0777, 0, 0, {}, "x");
    }
    append_tar_entry(tar, "aaaaaaaaa", '0', 0644, 0, 0, {'x'});
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

std::vector<uint8_t> make_many_inode_tar() {
    std::vector<uint8_t> tar;
    constexpr size_t kDirs = 11;
    constexpr size_t kFilesPerDir = 3000;
    tar.reserve((kDirs + kDirs * kFilesPerDir + 2) * 512);
    for (size_t d = 0; d < kDirs; ++d) {
        const std::string dir = "d" + std::to_string(d);
        append_tar_entry(tar, dir + "/", '5', 0755, 0, 0);
        for (size_t f = 0; f < kFilesPerDir; ++f) {
            append_tar_entry(tar, dir + "/f" + std::to_string(f), '0', 0644, 0, 0);
        }
    }
    tar.resize(tar.size() + 1024, 0);
    return tar;
}

std::string write_sparse_regular_files_tar(const std::string& path, size_t count,
                                           uint64_t payload_size) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    const uint64_t padded_payload = ((payload_size + 511) / 512) * 512;
    for (size_t i = 0; i < count; ++i) {
        const auto h = make_tar_header("big" + std::to_string(i), '0', 0644,
                                       0, 0, payload_size);
        write_all_fd(fd, h.data(), h.size());
        REQUIRE(::lseek(fd, static_cast<off_t>(padded_payload), SEEK_CUR) >= 0);
    }
    std::array<uint8_t, 1024> end_blocks {};
    write_all_fd(fd, end_blocks.data(), end_blocks.size());
    REQUIRE(::close(fd) == 0);
    return path;
}

std::string file_sha256(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    common::Sha256 h;
    std::array<uint8_t, 65536> buf {};
    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        REQUIRE(r >= 0);
        if (r == 0) break;
        h.update(buf.data(), static_cast<size_t>(r));
    }
    REQUIRE(::close(fd) == 0);
    return h.final_hex();
}

struct Ext2Inode {
    uint16_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t size = 0;
    std::array<uint32_t, 15> blocks {};
};

class Ext2View {
public:
    explicit Ext2View(std::vector<uint8_t> raw) : raw_(std::move(raw)) {
        REQUIRE(raw_.size() >= 8192);
        const uint8_t* sb = raw_.data() + 1024;
        REQUIRE(bytes::load_u16_le(sb + 56) == 0xef53);
        block_size_ = 1024u << bytes::load_u32_le(sb + 24);
        REQUIRE(block_size_ == 4096);
        inode_size_ = bytes::load_u16_le(sb + 88);
        REQUIRE(inode_size_ >= 128);
        ro_compat_ = bytes::load_u32_le(sb + 100);
        const uint8_t* gd = raw_.data() + block_size_;
        inode_table_block_ = bytes::load_u32_le(gd + 8);
    }

    Ext2Inode inode(uint32_t ino) const {
        const uint64_t off = static_cast<uint64_t>(inode_table_block_) * block_size_ +
                             static_cast<uint64_t>(ino - 1) * inode_size_;
        REQUIRE(off + inode_size_ <= raw_.size());
        const uint8_t* p = raw_.data() + off;
        Ext2Inode n;
        n.mode = bytes::load_u16_le(p + 0);
        n.uid = bytes::load_u16_le(p + 2) |
                (static_cast<uint32_t>(bytes::load_u16_le(p + 120)) << 16);
        n.size = bytes::load_u32_le(p + 4);
        n.gid = bytes::load_u16_le(p + 24) |
                (static_cast<uint32_t>(bytes::load_u16_le(p + 122)) << 16);
        for (size_t i = 0; i < n.blocks.size(); ++i) {
            n.blocks[i] = bytes::load_u32_le(p + 40 + i * 4);
        }
        return n;
    }

    std::map<std::string, uint32_t> list_dir(uint32_t ino) const {
        const Ext2Inode dir = inode(ino);
        std::map<std::string, uint32_t> out;
        for (uint32_t block : data_blocks(dir)) {
            REQUIRE(block != 0);
            const uint64_t base = static_cast<uint64_t>(block) * block_size_;
            REQUIRE(base + block_size_ <= raw_.size());
            uint32_t pos = 0;
            while (pos < block_size_) {
                const uint8_t* e = raw_.data() + base + pos;
                const uint32_t child_ino = bytes::load_u32_le(e);
                const uint16_t rec_len = bytes::load_u16_le(e + 4);
                const uint8_t name_len = e[6];
                REQUIRE(rec_len >= 8);
                REQUIRE(pos + rec_len <= block_size_);
                REQUIRE(name_len <= rec_len - 8);
                if (child_ino != 0) {
                    out.emplace(std::string(reinterpret_cast<const char*>(e + 8), name_len),
                                child_ino);
                }
                pos += rec_len;
            }
        }
        return out;
    }

    uint32_t lookup(std::initializer_list<std::string> path) const {
        uint32_t ino = 2;
        for (const auto& part : path) {
            const auto entries = list_dir(ino);
            auto it = entries.find(part);
            REQUIRE(it != entries.end());
            ino = it->second;
        }
        return ino;
    }

    std::vector<uint8_t> read_file(uint32_t ino) const {
        const Ext2Inode file = inode(ino);
        std::vector<uint8_t> out;
        out.reserve(file.size);
        auto append_block = [&](uint32_t block) {
            const size_t want = std::min<size_t>(block_size_, file.size - out.size());
            if (block == 0) {
                out.insert(out.end(), want, 0);
                return;
            }
            const uint64_t base = static_cast<uint64_t>(block) * block_size_;
            REQUIRE(base + block_size_ <= raw_.size());
            out.insert(out.end(), raw_.begin() + base, raw_.begin() + base + want);
        };
        for (uint32_t block : data_blocks(file)) {
            append_block(block);
        }
        REQUIRE(out.size() == file.size);
        return out;
    }

    std::string inline_symlink(uint32_t ino) const {
        const uint64_t off = static_cast<uint64_t>(inode_table_block_) * block_size_ +
                             static_cast<uint64_t>(ino - 1) * inode_size_ + 40;
        const Ext2Inode link = inode(ino);
        REQUIRE(link.size <= 60);
        return std::string(reinterpret_cast<const char*>(raw_.data() + off), link.size);
    }

    std::string symlink_target(uint32_t ino) const {
        const Ext2Inode link = inode(ino);
        if (link.size < 60) return inline_symlink(ino);
        const auto bytes = read_file(ino);
        return std::string(bytes.begin(), bytes.end());
    }

    uint32_t ro_compat_features() const { return ro_compat_; }

private:
    std::vector<uint32_t> data_blocks(const Ext2Inode& n) const {
        const uint64_t block_count = (n.size + block_size_ - 1) / block_size_;
        std::vector<uint32_t> out;
        out.reserve(static_cast<size_t>(std::min<uint64_t>(block_count, 4096)));
        auto add_block = [&](uint32_t block) {
            if (out.size() < block_count) out.push_back(block);
        };
        auto visit_indirect = [&](auto&& self, uint32_t block, unsigned level) -> void {
            if (out.size() >= block_count) return;
            if (level == 0) {
                add_block(block);
                return;
            }
            if (block == 0) {
                for (size_t i = 0; i < kExt2PointersPerBlock && out.size() < block_count; ++i) {
                    self(self, 0, level - 1);
                }
                return;
            }
            const uint64_t base = static_cast<uint64_t>(block) * block_size_;
            REQUIRE(base + block_size_ <= raw_.size());
            for (size_t off = 0; off < block_size_ && out.size() < block_count; off += 4) {
                self(self, bytes::load_u32_le(raw_.data() + base + off), level - 1);
            }
        };
        for (size_t i = 0; i < 12 && out.size() < block_count; ++i) add_block(n.blocks[i]);
        if (out.size() < block_count) visit_indirect(visit_indirect, n.blocks[12], 1);
        if (out.size() < block_count) visit_indirect(visit_indirect, n.blocks[13], 2);
        if (out.size() < block_count) visit_indirect(visit_indirect, n.blocks[14], 3);
        REQUIRE(out.size() == block_count);
        return out;
    }

    std::vector<uint8_t> raw_;
    uint32_t block_size_ = 0;
    uint32_t inode_size_ = 0;
    uint32_t inode_table_block_ = 0;
    uint32_t ro_compat_ = 0;
};

std::vector<uint8_t> read_layer_raw(const std::string& path) {
    return test::run_coro([&]() -> elio::coro::task<std::vector<uint8_t>> {
        auto local = co_await source::LocalFileSource::open(path);
        source::BlobSourcePtr base = std::move(local);
        auto layer = co_await format::LsmtLayer::open(std::move(base));
        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
        layers.push_back(std::move(layer));
        auto merged = co_await format::MergedLsmt::open(std::move(layers));
        std::vector<uint8_t> raw(merged->size());
        const ssize_t r = co_await merged->pread(raw.data(), raw.size(), 0);
        REQUIRE(r == static_cast<ssize_t>(raw.size()));
        co_return raw;
    });
}

void assert_rootfs_metadata(const std::string& layer_path) {
    Ext2View fs(read_layer_raw(layer_path));
    const uint32_t etc_ino = fs.lookup({"etc"});
    const auto etc = fs.inode(etc_ino);
    REQUIRE((etc.mode & 0170000) == 0040000);
    REQUIRE((etc.mode & 07777) == 0750);
    REQUIRE(etc.uid == 5);
    REQUIRE(etc.gid == 6);

    const uint32_t hello_ino = fs.lookup({"etc", "hello.txt"});
    const auto hello = fs.inode(hello_ino);
    REQUIRE((hello.mode & 0170000) == 0100000);
    REQUIRE((hello.mode & 07777) == 0640);
    REQUIRE(hello.uid == 1000);
    REQUIRE(hello.gid == 1001);
    const auto hello_bytes = fs.read_file(hello_ino);
    REQUIRE(std::string(hello_bytes.begin(), hello_bytes.end()) == "hello\n");

    const uint32_t tool_ino = fs.lookup({"bin", "tool"});
    REQUIRE(fs.read_file(tool_ino) == test::pattern_bytes(kIndirectToolSize, 77));

    const uint32_t link_ino = fs.lookup({"link-to-hello"});
    const auto link = fs.inode(link_ino);
    REQUIRE((link.mode & 0170000) == 0120000);
    REQUIRE(link.uid == 7);
    REQUIRE(link.gid == 8);
    REQUIRE(fs.inline_symlink(link_ino) == "etc/hello.txt");

    const auto zero_dir = fs.inode(fs.lookup({"zero-dir"}));
    REQUIRE((zero_dir.mode & 0170000) == 0040000);
    REQUIRE((zero_dir.mode & 07777) == 0000);
    REQUIRE(zero_dir.uid == 9);
    REQUIRE(zero_dir.gid == 10);

    const uint32_t zero_file_ino = fs.lookup({"zero-file"});
    const auto zero_file = fs.inode(zero_file_ino);
    REQUIRE((zero_file.mode & 0170000) == 0100000);
    REQUIRE((zero_file.mode & 07777) == 0000);
    REQUIRE(zero_file.uid == 11);
    REQUIRE(zero_file.gid == 12);
    REQUIRE(fs.read_file(zero_file_ino) == std::vector<uint8_t>{'z'});

    const uint32_t zero_link_ino = fs.lookup({"zero-link"});
    const auto zero_link = fs.inode(zero_link_ino);
    REQUIRE((zero_link.mode & 0170000) == 0120000);
    REQUIRE((zero_link.mode & 07777) == 0000);
    REQUIRE(zero_link.uid == 13);
    REQUIRE(zero_link.gid == 14);
    REQUIRE(fs.inline_symlink(zero_link_ino) == "etc/hello.txt");
}

}  // namespace

// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obd-convert builds a deterministic ext2 layer from tar", "[cli]") {
    TempDir dir;
    const auto tar = make_rootfs_tar();
    const std::string tar_path = test::write_file(dir / "rootfs.tar", tar);
    const std::string out_a = dir / "a";
    const std::string out_b = dir / "b";

    const auto a = run_convert({"--input", tar_path, "--out-dir", out_a,
                                "--name", "rootfs"});
    REQUIRE(a.exit_code == 0);
    const auto b = run_convert({"--input", "-", "--out-dir", out_b,
                                "--name", "rootfs"}, &tar);
    REQUIRE(b.exit_code == 0);
    const uint64_t explicit_size = 8ull * 1024 * 1024;
    const std::string out_sized = dir / "sized";
    const auto sized = run_convert({"--input", tar_path, "--out-dir", out_sized,
                                    "--name", "rootfs", "--size",
                                    std::to_string(explicit_size)});
    REQUIRE(sized.exit_code == 0);

    const auto ja = nlohmann::json::parse(a.out);
    const auto jb = nlohmann::json::parse(b.out);
    const auto js = nlohmann::json::parse(sized.out);
    REQUIRE(ja["converter"]["backend"].get<std::string>() == "builtin-ext2");
    REQUIRE(ja["converter"]["filesystem"].get<std::string>() == "ext2");
    REQUIRE(js["converter"]["virtual_size"].get<uint64_t>() == explicit_size);
    const std::string layer_a = ja["lowers"][0]["file"].get<std::string>();
    const std::string layer_b = jb["lowers"][0]["file"].get<std::string>();
    REQUIRE(file_sha256(layer_a) == file_sha256(layer_b));
    REQUIRE(ja["lowers"][0]["digest"].get<std::string>() ==
            "sha256:" + file_sha256(layer_a));

    assert_rootfs_metadata(layer_a);
}


// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obd-convert defaults to libe2fs when the backend is enabled", "[cli]") {
    TempDir dir;
    const auto tar = make_rootfs_tar();
    const std::string tar_path = test::write_file(dir / "rootfs.tar", tar);
    const std::string out_a = dir / "default-a";
    const std::string out_b = dir / "default-b";

    const auto a = run_convert({"--input", tar_path, "--out-dir", out_a,
                                "--name", "rootfs"}, nullptr, false);
    REQUIRE(a.exit_code == 0);
#if OBD_TEST_HAVE_LIBE2FS
    std::this_thread::sleep_for(std::chrono::seconds(2));
#endif
    const auto b = run_convert({"--input", "-", "--out-dir", out_b,
                                "--name", "rootfs"}, &tar, false);
    REQUIRE(b.exit_code == 0);

    const auto ja = nlohmann::json::parse(a.out);
    const auto jb = nlohmann::json::parse(b.out);
#if OBD_TEST_HAVE_LIBE2FS
    REQUIRE(ja["converter"]["backend"].get<std::string>() == "libe2fs");
#else
    REQUIRE(ja["converter"]["backend"].get<std::string>() == "builtin-ext2");
#endif
    const std::string layer_a = ja["lowers"][0]["file"].get<std::string>();
    const std::string layer_b = jb["lowers"][0]["file"].get<std::string>();
    REQUIRE(file_sha256(layer_a) == file_sha256(layer_b));
    assert_rootfs_metadata(layer_a);
}

// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obd-convert libe2fs expands built-in file and directory limits", "[cli]") {
#if OBD_TEST_HAVE_LIBE2FS
    TempDir dir;
    const std::string large_tar = write_sparse_regular_files_tar(
        dir / "large.tar", 1, kMaxBuiltInFileBytes + kExt2BlockSize);
    const std::string large_dir = dir / "large";
    const auto large = run_convert({"--input", large_tar, "--out-dir", large_dir,
                                    "--name", "large"}, nullptr, false);
    REQUIRE(large.exit_code == 0);
    const auto large_manifest = nlohmann::json::parse(large.out);
    REQUIRE(large_manifest["converter"]["backend"].get<std::string>() == "libe2fs");
    const std::string large_layer = large_manifest["lowers"][0]["file"].get<std::string>();
    Ext2View large_fs(read_layer_raw(large_layer));
    const auto large_bytes = large_fs.read_file(large_fs.lookup({"big0"}));
    REQUIRE(large_bytes.size() == kMaxBuiltInFileBytes + kExt2BlockSize);
    REQUIRE(std::all_of(large_bytes.begin(), large_bytes.end(), [](uint8_t b) { return b == 0; }));

    const auto builtin_large = run_convert({"--input", large_tar, "--out-dir", dir / "builtin-large",
                                            "--name", "large", "--backend", "builtin-ext2"},
                                           nullptr, false);
    REQUIRE(builtin_large.exit_code == 1);
    REQUIRE(builtin_large.err.find("file is too large") != std::string::npos);

    const auto too_small = run_convert({"--input", large_tar, "--out-dir", dir / "too-small-large",
                                        "--name", "large", "--size",
                                        std::to_string(kMaxBuiltInFileBytes + kExt2BlockSize)},
                                       nullptr, false);
    REQUIRE(too_small.exit_code == 1);
    REQUIRE(too_small.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);

    const uint64_t explicit_capacity_size = 8ull * 1024 * 1024;
    const std::string near_capacity_tar = write_sparse_regular_files_tar(
        dir / "near-capacity.tar", 1, 2028ull * kExt2BlockSize);
    const auto near_capacity =
        run_convert({"--input", near_capacity_tar, "--out-dir", dir / "near-capacity",
                     "--name", "near-capacity", "--size",
                     std::to_string(explicit_capacity_size)},
                    nullptr, false);
    REQUIRE(near_capacity.exit_code == 0);
    const auto near_capacity_manifest = nlohmann::json::parse(near_capacity.out);
    REQUIRE(near_capacity_manifest["converter"]["virtual_size"].get<uint64_t>() ==
            explicit_capacity_size);

    const auto feature_tar = make_libe2fs_feature_tar();
    const std::string feature_path = test::write_file(dir / "features.tar", feature_tar);
    const auto feature_result = run_convert({"--input", feature_path, "--out-dir", dir / "features",
                                             "--name", "features"}, nullptr, false);
    REQUIRE(feature_result.exit_code == 0);
    const auto feature_manifest = nlohmann::json::parse(feature_result.out);
    Ext2View feature_fs(read_layer_raw(feature_manifest["lowers"][0]["file"].get<std::string>()));
    REQUIRE((feature_fs.ro_compat_features() & kExt2FeatureRoCompatLargeFile) != 0);
    const auto wide_owner = feature_fs.inode(feature_fs.lookup({"wide-owner"}));
    REQUIRE(wide_owner.uid == 70000);
    REQUIRE(wide_owner.gid == 80000);
    REQUIRE(feature_fs.symlink_target(feature_fs.lookup({"slow-link"})) == std::string(60, 't'));

    const auto slow_symlink_too_small =
        run_convert({"--input", feature_path, "--out-dir", dir / "slow-link-too-small",
                     "--name", "features", "--size", "40960"}, nullptr, false);
    REQUIRE(slow_symlink_too_small.exit_code == 1);
    REQUIRE(slow_symlink_too_small.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);

    const auto tiny_tar = make_tiny_file_tar();
    const std::string tiny_path = test::write_file(dir / "tiny.tar", tiny_tar);
    const auto min_group_too_small =
        run_convert({"--input", tiny_path, "--out-dir", dir / "min-group-too-small",
                     "--name", "tiny", "--size", std::to_string(13ull * kExt2BlockSize)},
                    nullptr, false);
    REQUIRE(min_group_too_small.exit_code == 1);
    REQUIRE(min_group_too_small.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);

    const auto many = make_many_root_files_tar(4100);
    const std::string many_tar = test::write_file(dir / "many.tar", many);
    const auto many_result = run_convert({"--input", many_tar, "--out-dir", dir / "many",
                                          "--name", "many"}, nullptr, false);
    REQUIRE(many_result.exit_code == 0);
    const auto many_manifest = nlohmann::json::parse(many_result.out);
    REQUIRE(many_manifest["converter"]["backend"].get<std::string>() == "libe2fs");
    const std::string many_layer = many_manifest["lowers"][0]["file"].get<std::string>();
    Ext2View many_fs(read_layer_raw(many_layer));
    REQUIRE(many_fs.lookup({"f4099"}) != 0);
#else
    SUCCEED("libe2fs backend disabled in this build");
#endif
}

// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obd-convert atomically replaces existing output symlinks", "[cli]") {
    TempDir dir;
    const auto tar = make_rootfs_tar();
    const std::string out_dir = dir / "out";
    REQUIRE(::mkdir(out_dir.c_str(), 0755) == 0);
    const std::vector<uint8_t> victim_bytes = {'k', 'e', 'e', 'p'};
    const std::string victim = test::write_file(dir / "victim", victim_bytes);
    const std::string victim_sha = file_sha256(victim);
    const std::string lsmt_path = out_dir + "/rootfs.lsmt";
    const std::string raw_path = out_dir + "/.rootfs.ext2.tmp";
    REQUIRE(::symlink(victim.c_str(), lsmt_path.c_str()) == 0);
    REQUIRE(::symlink(victim.c_str(), raw_path.c_str()) == 0);

    const auto result = run_convert({"--input", "-", "--out-dir", out_dir,
                                     "--name", "rootfs", "--keep-raw"}, &tar);
    REQUIRE(result.exit_code == 0);
    REQUIRE(file_sha256(victim) == victim_sha);

    struct stat st {};
    REQUIRE(::lstat(lsmt_path.c_str(), &st) == 0);
    REQUIRE(S_ISREG(st.st_mode));
    REQUIRE(::lstat(raw_path.c_str(), &st) == 0);
    REQUIRE(S_ISREG(st.st_mode));
}

// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obd-convert reports usage errors with exit 2", "[cli]") {
    const auto unknown = run_convert({"--unknown"});
    REQUIRE(unknown.exit_code == 2);
    REQUIRE(unknown.err.find("unknown argument") != std::string::npos);

    const auto missing = run_convert({"--input"});
    REQUIRE(missing.exit_code == 2);
    REQUIRE(missing.err.find("missing value for --input") != std::string::npos);

    const auto bad_size =
        run_convert({"--input", "rootfs.tar", "--out-dir", "out", "--size", "abc"});
    REQUIRE(bad_size.exit_code == 2);
    REQUIRE(bad_size.err.find("--size must be a positive integer") !=
            std::string::npos);

    const auto bad_backend =
        run_convert({"--input", "rootfs.tar", "--out-dir", "out",
                     "--backend", "bogus"});
    REQUIRE(bad_backend.exit_code == 2);
    REQUIRE(bad_backend.err.find("--backend must be builtin-ext2 or libe2fs") !=
            std::string::npos);

#if !OBD_TEST_HAVE_LIBE2FS
    const auto disabled_libe2fs =
        run_convert({"--input", "rootfs.tar", "--out-dir", "out",
                     "--backend", "libe2fs"});
    REQUIRE(disabled_libe2fs.exit_code == 2);
    REQUIRE(disabled_libe2fs.err.find("--backend libe2fs requires a build with") !=
            std::string::npos);
#endif
}

// NOTE: name on one source line (check-docs extracts names line-wise).
TEST_CASE("cli: obd-convert rejects unsupported tar entries before writing a layer", "[cli]") {
    TempDir dir;
    std::vector<uint8_t> tar;
    append_tar_entry(tar, "dev/null", '3', 0600, 0, 0);
    tar.resize(tar.size() + 1024, 0);
    const std::string tar_path = test::write_file(dir / "bad.tar", tar);
    const std::string out_dir = dir / "out";

    const auto result = run_convert({"--input", tar_path, "--out-dir", out_dir,
                                     "--name", "bad"});
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.err.find("unsupported tar entry type") != std::string::npos);
    struct stat st {};
    REQUIRE(::stat((out_dir + "/bad.lsmt").c_str(), &st) != 0);

    const std::string empty_path = test::write_file(dir / "empty.tar", {});
    const std::string empty_dir = dir / "empty";
    const auto empty_result = run_convert({"--input", empty_path, "--out-dir",
                                           empty_dir, "--name", "empty"});
    REQUIRE(empty_result.exit_code == 1);
    REQUIRE(empty_result.err.find("empty tar stream") != std::string::npos);
    REQUIRE(::stat((empty_dir + "/empty.lsmt").c_str(), &st) != 0);

    std::vector<uint8_t> empty_end_marker(1024, 0);
    const std::string empty_end_path =
        test::write_file(dir / "empty-end-marker.tar", empty_end_marker);
    const std::string empty_end_dir = dir / "empty-end";
    const auto empty_end_result =
        run_convert({"--input", empty_end_path, "--out-dir", empty_end_dir,
                     "--name", "empty"});
    REQUIRE(empty_end_result.exit_code == 1);
    REQUIRE(empty_end_result.err.find("empty tar stream") != std::string::npos);
    REQUIRE(::stat((empty_end_dir + "/empty.lsmt").c_str(), &st) != 0);

    std::vector<uint8_t> legacy_tar;
    append_tar_entry(legacy_tar, "legacy-file", '0', 0644, 0, 0);
    std::fill(legacy_tar.begin() + 257, legacy_tar.begin() + 265, 0);
    rewrite_tar_checksum(legacy_tar, 0);
    legacy_tar.resize(legacy_tar.size() + 1024, 0);
    const std::string legacy_path = test::write_file(dir / "legacy.tar", legacy_tar);
    const std::string legacy_dir = dir / "legacy";
    const auto legacy_result = run_convert({"--input", legacy_path, "--out-dir",
                                            legacy_dir, "--name", "legacy"});
    REQUIRE(legacy_result.exit_code == 1);
    REQUIRE(legacy_result.err.find("expected ustar magic and version") !=
            std::string::npos);
    REQUIRE(::stat((legacy_dir + "/legacy.lsmt").c_str(), &st) != 0);

    std::vector<uint8_t> single_zero(512, 0);
    const std::string single_zero_path =
        test::write_file(dir / "single-zero.tar", single_zero);
    const std::string single_zero_dir = dir / "single-zero";
    const auto single_zero_result =
        run_convert({"--input", single_zero_path, "--out-dir", single_zero_dir,
                     "--name", "bad"});
    REQUIRE(single_zero_result.exit_code == 1);
    REQUIRE(single_zero_result.err.find("two zero blocks") != std::string::npos);
    REQUIRE(::stat((single_zero_dir + "/bad.lsmt").c_str(), &st) != 0);

    std::vector<uint8_t> zero_then_junk(1024, 0);
    zero_then_junk[512] = 1;
    const std::string zero_junk_path =
        test::write_file(dir / "zero-junk.tar", zero_then_junk);
    const std::string zero_junk_dir = dir / "zero-junk";
    const auto zero_junk_result =
        run_convert({"--input", zero_junk_path, "--out-dir", zero_junk_dir,
                     "--name", "bad"});
    REQUIRE(zero_junk_result.exit_code == 1);
    REQUIRE(zero_junk_result.err.find("two zero blocks") != std::string::npos);
    REQUIRE(::stat((zero_junk_dir + "/bad.lsmt").c_str(), &st) != 0);

    const auto tiny_tar = make_tiny_file_tar();
    const std::string tiny_tar_path = test::write_file(dir / "tiny.tar", tiny_tar);
    const std::string too_small_dir = dir / "too-small";
    const auto too_small = run_convert({"--input", tiny_tar_path, "--out-dir",
                                        too_small_dir, "--name", "small",
                                        "--size", "36864"});
    REQUIRE(too_small.exit_code == 1);
    REQUIRE(too_small.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);
    REQUIRE(::stat((too_small_dir + "/small.lsmt").c_str(), &st) != 0);

    const auto file_then_dir_tar = make_file_then_directory_budget_tar();
    const std::string file_then_dir_path =
        test::write_file(dir / "file-then-dir.tar", file_then_dir_tar);
    const std::string file_then_dir_dir = dir / "file-then-dir";
    const auto file_then_dir =
        run_convert({"--input", file_then_dir_path, "--out-dir",
                     file_then_dir_dir, "--name", "file-then-dir",
                     "--size", "40960"});
    REQUIRE(file_then_dir.exit_code == 1);
    REQUIRE(file_then_dir.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);
    REQUIRE(::stat((file_then_dir_dir + "/file-then-dir.lsmt").c_str(), &st) != 0);

    const std::vector<uint8_t> missing_end_tar(tiny_tar.begin(),
                                               tiny_tar.end() - 1024);
    const std::string missing_end_path =
        test::write_file(dir / "missing-end.tar", missing_end_tar);
    const std::string missing_end_dir = dir / "missing-end";
    const auto missing_end =
        run_convert({"--input", missing_end_path, "--out-dir", missing_end_dir,
                     "--name", "missing"});
    REQUIRE(missing_end.exit_code == 1);
    REQUIRE(missing_end.err.find("missing end-of-archive marker") !=
            std::string::npos);
    REQUIRE(::stat((missing_end_dir + "/missing.lsmt").c_str(), &st) != 0);

    const std::string oversized_tar = write_sparse_regular_files_tar(
        dir / "oversized.tar", 1, kMaxBuiltInFileBytes + 1);
    const std::string oversized_dir = dir / "oversized";
    const auto oversized = run_convert({"--input", oversized_tar, "--out-dir",
                                        oversized_dir, "--name", "oversized"});
    REQUIRE(oversized.exit_code == 1);
    REQUIRE(oversized.err.find("file is too large") != std::string::npos);
    REQUIRE(::stat((oversized_dir + "/oversized.lsmt").c_str(), &st) != 0);

    const std::string explicit_budget_tar = write_sparse_regular_files_tar(
        dir / "explicit-budget.tar", 2, 4ull * 1024 * 1024);
    const std::string explicit_budget_dir = dir / "explicit-budget";
    const auto explicit_budget =
        run_convert({"--input", explicit_budget_tar, "--out-dir",
                     explicit_budget_dir, "--name", "explicit-budget",
                     "--size", "8388608"});
    REQUIRE(explicit_budget.exit_code == 1);
    REQUIRE(explicit_budget.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);
    REQUIRE(::stat((explicit_budget_dir + "/explicit-budget.lsmt").c_str(),
                   &st) != 0);

    const std::string aggregate_tar = write_sparse_regular_files_tar(
        dir / "aggregate.tar", 32, 4ull * 1024 * 1024);
    const std::string aggregate_dir = dir / "aggregate";
    const auto aggregate = run_convert({"--input", aggregate_tar, "--out-dir",
                                        aggregate_dir, "--name", "aggregate"});
    REQUIRE(aggregate.exit_code == 1);
    REQUIRE(aggregate.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);
    REQUIRE(::stat((aggregate_dir + "/aggregate.lsmt").c_str(), &st) != 0);

    const auto sorted_budget_tar = make_sorted_directory_budget_tar();
    const std::string sorted_budget_path =
        test::write_file(dir / "sorted-budget.tar", sorted_budget_tar);
    const std::string sorted_budget_dir = dir / "sorted-budget";
    const auto sorted_budget =
        run_convert({"--input", sorted_budget_path, "--out-dir",
                     sorted_budget_dir, "--name", "sorted-budget",
                     "--size", std::to_string(15ull * kExt2BlockSize)});
    REQUIRE(sorted_budget.exit_code == 1);
    REQUIRE(sorted_budget.err.find("contents and ext2 metadata exceed image budget") !=
            std::string::npos);
    REQUIRE(::stat((sorted_budget_dir + "/sorted-budget.lsmt").c_str(),
                   &st) != 0);

    const std::string many_dir = dir / "many-dir";
    const auto many = make_many_root_files_tar(4100);
    const std::string many_tar = test::write_file(dir / "many.tar", many);
    const auto dir_result = run_convert({"--input", many_tar, "--out-dir",
                                         many_dir, "--name", "many"});
    REQUIRE(dir_result.exit_code == 1);
    REQUIRE(dir_result.err.find("directories up to 12 data blocks") !=
            std::string::npos);
    REQUIRE(::stat((many_dir + "/many.lsmt").c_str(), &st) != 0);

    const std::string many_inode_dir = dir / "many-inodes";
    const auto many_inodes = make_many_inode_tar();
    const std::string many_inode_tar = test::write_file(dir / "many-inodes.tar",
                                                        many_inodes);
    const auto inode_result = run_convert({"--input", many_inode_tar, "--out-dir",
                                           many_inode_dir, "--name", "many"});
    REQUIRE(inode_result.exit_code == 1);
    REQUIRE(inode_result.err.find("at most 32768 inodes") != std::string::npos);
    REQUIRE(::stat((many_inode_dir + "/many.lsmt").c_str(), &st) != 0);
}
