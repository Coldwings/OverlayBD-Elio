// Test helper for sparse-upper process-crash recovery. It deliberately exits
// without unwinding after one completed operation, so no layer/runtime
// destructor or explicit guest flush participates in publication.
#include "format/sparse_rw.hpp"

#include <elio/runtime/async_main.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

bool parse_u64(const char* text, uint64_t* value) {
    if (text == nullptr || *text == '\0' || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) return 64;
    const std::string path = argv[1];
    const std::string operation = argv[2];
    uint64_t vsize = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!parse_u64(argv[3], &vsize) || !parse_u64(argv[4], &offset) ||
        !parse_u64(argv[5], &length) ||
        length > std::numeric_limits<size_t>::max()) {
        return 64;
    }

    try {
        return elio::run([path, operation, vsize, offset,
                          length]() -> elio::coro::task<int> {
            auto layer =
                co_await obd::format::SparseRwLayer::open(path, vsize);
            int rc = -EINVAL;
            if (operation == "write") {
                std::vector<uint8_t> data(static_cast<size_t>(length), 0xa5);
                const ssize_t written =
                    co_await layer->pwrite(data.data(), data.size(), offset);
                rc = written == static_cast<ssize_t>(data.size()) ? 0
                                                                  : -EIO;
            } else if (operation == "discard") {
                rc = co_await layer->discard(offset, length);
            }
            ::_exit(rc == 0 ? 0 : 65);
        });
    } catch (...) {
        return 66;
    }
}
