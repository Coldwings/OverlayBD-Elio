#include "format/sparse_export_output.hpp"
#include "format/sparse_export_fault.hpp"
#include "common/errors.hpp"
#include <array>
#include <filesystem>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace obd::format::sparse_export_detail {
namespace {
bool same_inode(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
std::filesystem::path normalized_name(const std::filesystem::path& path) {
    // Resolve the directory through the filesystem before normalizing: a
    // symlink followed by ".." must use the target directory's parent.
    const auto full = std::filesystem::absolute(path);
    return std::filesystem::canonical(full.parent_path()) / full.filename();
}
}
Output::~Output() {
    if (existing_fd_ >= 0) ::close(existing_fd_);
    if (fd_ >= 0) ::close(fd_);
    if (directory_fd_ >= 0) {
        if (!temporary_.empty()) ::unlinkat(directory_fd_, temporary_.c_str(), 0);
        ::close(directory_fd_);
    }
}
std::unique_ptr<Output> Output::prepare(const std::string& source_path,
    int data_fd, int coverage_fd, const std::string& destination) {
    if (destination.empty() || destination.find('\0') != std::string::npos ||
        source_path.find('\0') != std::string::npos)
        throw error(EINVAL, "sparse export: invalid pathname");
    const auto target = normalized_name(destination);
    for (const auto* suffix : {"", ".coverage", ".coverage.tmp", ".zeroes", ".zeroes.tmp"}) {
        if (target == normalized_name(source_path + suffix))
            throw error(EINVAL, "sparse export: destination is a source artifact");
    }
    auto output = std::unique_ptr<Output>(new Output);
    output->target_ = target.filename().string();
    output->directory_fd_ = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (output->directory_fd_ < 0) throw_errno(errno, "sparse export: open destination directory");
    for (const auto fd : {data_fd, coverage_fd}) {
        struct stat source_stat{};
        if (fd < 0 || ::fstat(fd, &source_stat) != 0)
            throw error(EIO, "sparse export: source descriptor unavailable");
        output->source_inodes_.push_back(source_stat);
    }
    for (const auto* suffix : {".coverage.tmp", ".zeroes", ".zeroes.tmp"}) {
        struct stat metadata{};
        if (::stat((source_path + suffix).c_str(), &metadata) == 0)
            output->source_inodes_.push_back(metadata);
        else if (errno != ENOENT) throw_errno(errno, "sparse export: inspect source metadata");
    }
    struct stat destination_stat{};
    if (::fstatat(output->directory_fd_, output->target_.c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(destination_stat.st_mode))
            throw error(EINVAL, "sparse export: destination must be a regular file");
        for (const auto& source_stat : output->source_inodes_) {
            if (same_inode(source_stat, destination_stat))
                throw error(EINVAL, "sparse export: destination aliases source inode");
        }
    } else if (errno != ENOENT) throw_errno(errno, "sparse export: inspect destination");
    // Randomness is confined to an unpublished name, never artifact bytes.
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::array<unsigned char, 16> nonce{};
        size_t done = 0;
        while (done < nonce.size()) {
            const auto n = ::getrandom(nonce.data() + done, nonce.size() - done, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw error(n < 0 ? errno : EIO, "sparse export: temporary name");
            done += static_cast<size_t>(n);
        }
        std::string name = ".obd-sparse-export-";
        constexpr char hex[] = "0123456789abcdef";
        for (const auto b : nonce) { name += hex[b >> 4]; name += hex[b & 15]; }
        const int fd = ::openat(output->directory_fd_, name.c_str(),
                               O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            throw_errno(errno, "sparse export: create staging file");
        }
        output->temporary_.swap(name); // Nonthrowing ownership adoption.
        output->fd_ = fd;
        return output;
    }
    throw error(EEXIST, "sparse export: could not reserve staging name");
}
bool Output::publish() {
    check_fault(Fault::publish);
    if (::syscall(SYS_renameat2, directory_fd_, temporary_.c_str(),
                  directory_fd_, target_.c_str(), RENAME_NOREPLACE) == 0) {
        temporary_.clear();
        return true;
    }
    if (errno != EEXIST) throw_errno(errno, "sparse export: publish staging file");
    existing_fd_ = ::openat(directory_fd_, target_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (existing_fd_ < 0) throw_errno(errno, "sparse export: inspect existing output");
    struct stat current{}, staged{};
    if (::fstat(existing_fd_, &current) != 0 || ::fstat(fd_, &staged) != 0)
        throw_errno(errno, "sparse export: stat existing output");
    for (const auto& source_stat : source_inodes_)
        if (same_inode(source_stat, current))
            throw error(EINVAL, "sparse export: published target aliases source");
    if (!S_ISREG(current.st_mode) || current.st_size != staged.st_size)
        throw error(EEXIST, "sparse export: different output already exists");
    return false;
}
void Output::sync_directory() {
    // An identical-output retry no longer needs its private staging name.
    // Remove it before the directory durability boundary, not afterwards.
    if (!temporary_.empty()) {
        if (::unlinkat(directory_fd_, temporary_.c_str(), 0) != 0)
            throw_errno(errno, "sparse export: remove redundant staging file");
        temporary_.clear();
    }
    check_fault(Fault::directory_sync);
    if (::fsync(directory_fd_) != 0) throw_errno(errno, "sparse export: sync destination directory");
}
}  // namespace obd::format::sparse_export_detail
