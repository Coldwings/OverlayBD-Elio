// Existing Hybrid artifact-pair ownership and bounded asynchronous recovery.
#pragma once

#include "format/hybrid_format.hpp"
#include "format/hybrid_log_state.hpp"
#include <elio/coro/task.hpp>
#include <memory>
#include <atomic>
#include <string>

namespace obd::format::hybrid {

class FilePair {
public:
    ~FilePair();
    FilePair(const FilePair&) = delete;
    FilePair& operator=(const FilePair&) = delete;

    /// Never creates or truncates either artifact. The data descriptor holds
    /// a shared read lock or exclusive writer lock until destruction. Callers
    /// must drain operations before destruction, which releases both artifact
    /// locks before deferring descriptor close. Metadata
    /// filesystem calls run on the blocking pool; contents use native async IO.
    static elio::coro::task<std::unique_ptr<FilePair>> open_existing(
        std::string data_path, std::string index_path, bool writable = false);

    /// Atomically publishes a new directory containing data and index. The
    /// parent directory must exist; an existing destination is never replaced.
    static elio::coro::task<std::unique_ptr<FilePair>> create(
        std::string directory, uint64_t virtual_size);

    /// Syncs payload, writes a compact replacement log, syncs it, renames it
    /// over the owned index, and syncs the directory. A post-rename failure
    /// still adopts the published descriptor and byte count before returning.
    elio::coro::task<int> replace_index(std::vector<bytes::segment_mapping> mappings);
    elio::coro::task<int> sync_index_directory();
    elio::coro::task<int> append_log_progress(std::array<uint8_t, 16> record);
    elio::coro::task<int> commit_log_state();

    /// Blocking grow transaction; caller must run off scheduler workers.
    int grow(uint64_t virtual_size);
    elio::coro::task<int> seal(std::string user_tag = "");

    bool writable() const noexcept { return writable_; }
    int data_fd() const noexcept { return data_fd_; }
    int index_fd() const noexcept { return index_fd_; }
    uint64_t data_bytes() const noexcept { return data_bytes_; }
    uint64_t index_generation() const noexcept { return index_generation_; }
    uint64_t index_bytes() const noexcept { return index_bytes_; }
    const lsmt::HeaderTrailer& header() const noexcept { return header_; }
    const std::vector<bytes::segment_mapping>& mappings() const noexcept { return mappings_; }

private:
    FilePair() = default;
#ifdef OBD_TEST_HOOKS
    friend struct FilePairTestAccess;
    static std::atomic<int> replace_fail_stage_;
#endif
    elio::coro::task<void> log_operation(int operation, std::array<uint8_t, 16> record = {});
    void log_operation_sync(int operation, const std::array<uint8_t, 16>& record);
    std::optional<LogStamp> log_stamp_;
    int seal_sync(const std::string& user_tag);
    int replace_index_sync(const std::vector<bytes::segment_mapping>& mappings);
    static std::unique_ptr<FilePair> open_descriptors(
        const std::string& data_path, const std::string& index_path, bool writable);
    static std::unique_ptr<FilePair> create_sync(
        const std::string& directory, uint64_t virtual_size);
    bool writable_ = false;
    int data_fd_ = -1;
    int index_fd_ = -1;
    int index_directory_fd_ = -1;
    std::string index_name_;
    uint64_t data_bytes_ = 0;
    uint64_t index_bytes_ = 0;
    uint64_t index_generation_ = 0;
    lsmt::HeaderTrailer header_;
    std::vector<bytes::segment_mapping> mappings_;
};

}  // namespace obd::format::hybrid
