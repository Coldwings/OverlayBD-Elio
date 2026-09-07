// Common error types for overlaybd-elio.
//
// Cold paths (parsing, setup, control) report failures by throwing
// obd::error / obd::format_error. Hot IO paths (BlobSource::pread and the
// ublk data plane) return negative errno values instead of throwing; see
// docs/source.md for the per-interface contract.
#pragma once

#include <cerrno>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace obd {

/// Base class for all errors raised on setup/cold paths. Carries an errno
/// value so callers at a process boundary (e.g. the device process reporting
/// failure to its supervisor) can map it back to a system error.
class error : public std::system_error {
public:
    error(int err, std::string context)
        : std::system_error(err, std::generic_category(), std::move(context)),
          err_(err) {}

    int errno_value() const noexcept { return err_; }

private:
    int err_;
};

/// Raised when on-disk data violates the OverlayBD format: bad magic, bad
/// checksum, out-of-range index entries, truncated structures. Always maps
/// to EINVAL unless a more precise errno is given.
class format_error : public error {
public:
    explicit format_error(std::string context, int err = EINVAL)
        : error(err, "format error: " + std::move(context)) {}
};

[[noreturn]] inline void throw_errno(int err, std::string_view context) {
    throw error(err, std::string(context));
}

/// Throws obd::error with the current errno if `cond` is false.
inline void verify(bool cond, std::string_view context, int err = EINVAL) {
    if (!cond) throw_errno(err, context);
}

}  // namespace obd
