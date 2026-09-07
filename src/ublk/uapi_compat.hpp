// ublk uapi compatibility shim.
//
// Per AGENTS.md: ublk structures come from <linux/ublk_cmd.h> (kernel ABI);
// we never redefine what the header provides — only add what a given header
// version lacks, guarded by #ifndef. Baseline: kernel headers >= 6.0.
#pragma once

#include <linux/ublk_cmd.h>

#include <cstddef>
#include <cstdint>

// Older headers (< 6.0) lack the in-task completion flag we rely on.
#ifndef UBLK_F_URING_CMD_COMP_IN_TASK
#define UBLK_F_URING_CMD_COMP_IN_TASK (1ULL << 1)
#endif

namespace obd::ublk {

// Compile-time pinning of the kernel ABI we program against (ADR-0006
// cites these invariants; if a future kernel header changes them, the
// build must break loudly rather than corrupt the ring protocol).
static_assert(sizeof(ublksrv_io_desc) == 24,
              "ublksrv_io_desc must be 24 bytes (kernel ABI)");
static_assert(offsetof(ublksrv_io_desc, op_flags) == 0, "ABI");
static_assert(offsetof(ublksrv_io_desc, nr_sectors) == 4, "ABI");
static_assert(offsetof(ublksrv_io_desc, start_sector) == 8, "ABI");
static_assert(offsetof(ublksrv_io_desc, addr) == 16, "ABI");
static_assert(sizeof(ublksrv_io_cmd) == 16,
              "ublksrv_io_cmd must fit the 16B uring-cmd area");
static_assert(UBLK_MAX_QUEUE_DEPTH == 4096, "kernel ABI");

/// Per-queue mmap window stride: the driver maps queue q at
/// q_id * round_up(UBLK_MAX_QUEUE_DEPTH * io_desc_size, PAGE_SIZE)
/// (ublk_drv.c ublk_ch_mmap / ublk_max_cmd_buf_size).
constexpr uint64_t cmd_buf_stride(uint32_t io_desc_size) {
    const uint64_t page = 4096;
    const uint64_t bytes = UBLK_MAX_QUEUE_DEPTH * uint64_t{io_desc_size};
    return (bytes + page - 1) / page * page;
}

/// mmap length for one queue: round_up(depth * io_desc_size, PAGE_SIZE)
/// (must match exactly, or ublk_ch_mmap rejects the mapping).
constexpr uint64_t cmd_buf_size(uint16_t depth, uint32_t io_desc_size) {
    const uint64_t page = 4096;
    const uint64_t bytes = depth * uint64_t{io_desc_size};
    return (bytes + page - 1) / page * page;
}

}  // namespace obd::ublk
