// Deterministic test-only storage failure boundaries for sparse export.
#pragma once
#include "common/errors.hpp"
#ifdef OBD_TEST_HOOKS
#include <atomic>
#endif
namespace obd::format::sparse_export_detail {
enum class Fault { none, payload, index, header, trailer, file_sync, publish, directory_sync };
#ifdef OBD_TEST_HOOKS
inline std::atomic<Fault> fault_stage{Fault::none};
inline void fail_export_for_test(Fault stage) { fault_stage.store(stage); }
#endif
inline bool fault_at(Fault stage) {
#ifdef OBD_TEST_HOOKS
    return fault_stage.load() == stage;
#else
    (void)stage;
    return false;
#endif
}
inline void check_fault(Fault stage) {
    if (fault_at(stage)) throw error(EIO, "sparse export: injected storage failure");
}
}  // namespace obd::format::sparse_export_detail
