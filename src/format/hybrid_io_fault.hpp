#pragma once

namespace obd::format::hybrid {
#ifdef OBD_TEST_HOOKS
// Test-only EIO boundaries in native Writer IO. Reset with -1.
// 1=half payload written, 2=complete payload written, 3=before mapping publication,
// 4=half index record written, 5=complete index record before progress,
// 6=FLUSH data sync refused, 7=FLUSH index sync refused,
// 8=FLUSH directory sync refused.
void writer_io_fault_for_test(int stage);
#endif
}  // namespace obd::format::hybrid
