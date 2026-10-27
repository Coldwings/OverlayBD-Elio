#pragma once
#include <cstdint>

namespace obd::ublk {
#ifdef OBD_TEST_HOOKS
// Process-local observation for serial kernel tests. Enable only while no
// other fixture owns a device; disabling does not drain active requests.
void count_requests_for_test(bool enable);
uint64_t request_count_for_test(uint8_t operation);
#endif
}  // namespace obd::ublk
