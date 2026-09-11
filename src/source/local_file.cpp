// LocalFileSource. Reads go through the Elio IO backend (io_uring when
// available — required for regular files; see AGENTS.md build notes).
#include "source/local_file.hpp"

#include "common/errors.hpp"

#include <elio/io/io_awaitables.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace obd::source {

elio::coro::task<std::unique_ptr<LocalFileSource>> LocalFileSource::open(
    std::string path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw_errno(errno, "cannot open " + path);
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        throw_errno(e, "cannot stat " + path);
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        throw error(EINVAL, "not a regular file: " + path);
    }
    auto src = std::unique_ptr<LocalFileSource>(new LocalFileSource());
    src->fd_ = fd;
    src->size_.store(static_cast<uint64_t>(st.st_size),
                     std::memory_order_release);
    src->label_ = std::move(path);
    co_return src;
}

LocalFileSource::~LocalFileSource() {
    if (fd_ >= 0) {
        // Elio-safe close from a non-coroutine context: orders the fd close
        // against in-flight io_uring operations on the backend.
        elio::io::close_fd_for_destructor(fd_);
    }
}

elio::coro::task<ssize_t> LocalFileSource::pread(void* buf, size_t count,
                                                 uint64_t offset) {
    const uint64_t bound = size_.load(std::memory_order_acquire);
    if (offset >= bound) co_return 0;
    if (count > bound - offset) count = static_cast<size_t>(bound - offset);
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < count) {
        const auto r = co_await elio::io::async_read(
            fd_, p + done, count - done,
            static_cast<int64_t>(offset + done));
        if (r.result < 0) co_return r.result;
        if (r.result == 0) break;  // EOF (size_ changed underneath us)
        done += static_cast<size_t>(r.result);
    }
    co_return static_cast<ssize_t>(done);
}

}  // namespace obd::source
