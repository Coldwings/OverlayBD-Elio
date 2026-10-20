// Test admission only: no ublk commands or device creation.
#pragma once

#include <fcntl.h>
#include <linux/capability.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

namespace obd::test {

/// Pure classification shared with the unprivileged regression tests.
/// An empty reason admits the test. Unexpected probe failures remain errors.
inline std::string ublk_prerequisite_skip_reason(
    int control_open_error, uint64_t effective_capabilities) {
    switch (control_open_error) {
    case 0:
        break;
    case ENOENT:
    case ENODEV:
    case ENXIO:
        return "/dev/ublk-control unavailable (load the ublk driver)";
    case EACCES:
    case EPERM:
        return "/dev/ublk-control cannot be opened O_RDWR (permission denied)";
    default:
        throw std::system_error(control_open_error, std::system_category(),
                                "ublk test prerequisite: open /dev/ublk-control");
    }
    if ((effective_capabilities & (uint64_t{1} << CAP_SYS_ADMIN)) == 0) {
        return "ublk tests require effective CAP_SYS_ADMIN";
    }
    return {};
}

/// Call before allocating fixtures or starting a runtime. Use the same open
/// mode as Ctrl, not access(F_OK), which only proves that the node exists.
/// capget tests effective authority rather than UID: root may have dropped
/// the capability, and a nonroot caller may hold it. This is a necessary
/// admission check, not proof that later kernel commands will succeed.
inline std::string ublk_prerequisite_skip_reason() {
    int fd;
    do {
        fd = ::open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return ublk_prerequisite_skip_reason(errno, 0);
    if (::close(fd) != 0) {
        throw std::system_error(errno, std::system_category(),
                                "ublk test prerequisite: close control probe");
    }

    __user_cap_header_struct header {};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;  // Calling thread; no procfs or libcap dependency.
    __user_cap_data_struct capabilities[2] {};
    if (::syscall(SYS_capget, &header, capabilities) != 0) {
        throw std::system_error(errno, std::system_category(),
                                "ublk test prerequisite: capget");
    }
    const uint64_t effective = uint64_t{capabilities[0].effective} |
                              (uint64_t{capabilities[1].effective} << 32);
    return ublk_prerequisite_skip_reason(0, effective);
}

}  // namespace obd::test
