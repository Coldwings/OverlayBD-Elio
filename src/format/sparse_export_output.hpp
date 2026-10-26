// Blocking namespace operations for the sparse export transaction.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace obd::format::sparse_export_detail {
class Output {
public:
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    static std::unique_ptr<Output> prepare(const std::string& source_path,
        int data_fd, int coverage_fd, const std::string& destination);
    int fd() const noexcept { return fd_; }
    int existing_fd() const noexcept { return existing_fd_; }
    // False means an existing regular target must be compared byte-for-byte.
    // Never replaces a target. Throws on other failures.
    bool publish();
    void sync_directory();
private:
    Output() = default;
    int directory_fd_ = -1;
    int fd_ = -1;
    int existing_fd_ = -1;
    std::string target_;
    std::string temporary_;
    std::vector<struct stat> source_inodes_;
};
}  // namespace obd::format::sparse_export_detail
