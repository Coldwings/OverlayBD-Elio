// Owned upstream LSMT writable pair. Ordinary mapping publication is async;
// setup, index replacement and directory metadata run on the blocking pool.
#pragma once
#include "format/lsmt_rw_index.hpp"
#include <elio/coro/task.hpp>
#include <memory>
#include <string>

namespace obd::format::lsmt_rw {
#ifdef OBD_TEST_HOOKS
// 1=partial append; 2/3/4=fail data/index/directory FLUSH;
// 5/6/7=compact after data sync/new index sync/descriptor adoption.
// Process-exit injection is supported for compact stages; reset with -1.
void io_fault_for_test(int stage, bool exit_process = false);
// 1=intent published, 2=data header written, 3=index header written,
// 4=both headers synced, 5=intent retired. Reset with -1.
void grow_fault_for_test(int stage, bool exit_process = false);
// 1=data staged, 2=index staged, 3=intent published, 4=index published,
// 5=data published, 6=intent retired. Reset with -1.
void create_fault_for_test(int stage, bool exit_process = false);
// 1=legacy checkpoint validated, 2=index staged/synced, 3=index published,
// 4=directory synced. Reset with -1.
void migration_fault_for_test(int stage, bool exit_process = false);
#endif
class FilePair {
public:
    ~FilePair();
    FilePair(const FilePair&) = delete;
    FilePair& operator=(const FilePair&) = delete;
    static elio::coro::task<std::unique_ptr<FilePair>> open_existing(
        std::string data_path, std::string index_path);
    // New colocated artifacts only; existing names are never replaced. A
    // published creation intent may resume. The caller establishes durability
    // of the parent directory and its ancestors before creating the pair.
    static elio::coro::task<std::unique_ptr<FilePair>> create(
        std::string data_path, std::string index_path, uint64_t virtual_size);
    static elio::coro::task<std::unique_ptr<FilePair>> migrate_checkpoint(
        std::string data_path, std::string index_path);

    // Caller serializes all pair operations and completes payload writes first.
    // A failed append poisons this instance; reopen replays the completed prefix.
    elio::coro::task<int> append(bytes::segment_mapping mapping);
    elio::coro::task<int> flush();
    elio::coro::task<int> compact(std::vector<bytes::segment_mapping> live);
    // Blocking: caller runs on the blocking pool under its resize gate.
    int grow(uint64_t virtual_size);
    bool growth_pending() const noexcept { return growth_pending_; }
    bool healthy() const noexcept { return !poisoned_; }
    // After sealed replacement is published, keep the stable/index locks but
    // release the unlinked payload inode. The writer is already terminal.
    void retire_payload();
    int data_fd() const noexcept { return data_fd_; }
    uint64_t data_bytes() const noexcept { return data_bytes_; }
    uint64_t index_bytes() const noexcept { return index_bytes_; }
    const lsmt::HeaderTrailer& header() const noexcept { return header_; }
    const std::vector<bytes::segment_mapping>& mappings() const noexcept { return mappings_; }

private:
    FilePair() = default;
    static std::unique_ptr<FilePair> lock_workspace(const std::string&, const std::string&);
    static std::unique_ptr<FilePair> open_sync(const std::string&, const std::string&,
                                              std::unique_ptr<FilePair> = {});
    static std::unique_ptr<FilePair> create_sync(const std::string&, const std::string&, uint64_t);
    static std::unique_ptr<FilePair> migrate_sync(const std::string&, const std::string&);
    bool recover_creation(const std::string&, const std::string&);
    int compact_sync(const std::vector<bytes::segment_mapping>&);
    bool recover_grow();
    int data_fd_ = -1;
    int index_fd_ = -1;
    int lock_fd_ = -1;
    int directory_fd_ = -1;
    uint64_t data_bytes_ = 0;
    uint64_t index_bytes_ = 0;
    bool poisoned_ = false;
    bool growth_pending_ = false;
    std::string index_name_;
    lsmt::HeaderTrailer header_;
    std::vector<bytes::segment_mapping> mappings_;
};
}  // namespace obd::format::lsmt_rw
