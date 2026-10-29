// Test helper for sparse-upper process-crash recovery. It deliberately exits
// without unwinding after one completed operation, so no layer/runtime
// destructor or explicit guest flush participates in publication.
#include "format/sparse_rw.hpp"
#include "format/merged_writable.hpp"
#include "source/local_file.hpp"

#include <elio/runtime/async_main.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace obd::format {
struct SparseRwLayerTestAccess {
    static void extent_exit(int stage) { SparseRwLayer::extent_exit_stage_for_test_.store(stage); }
    static void fail_coverage_page(SparseRwLayer& layer, uint64_t page) {
        layer.fail_coverage_page_for_test_ = page;
    }
    static void interrupt_import_at(int stage) {
        SparseRwLayer::coverage_install_fail_stage_for_test_.store(
            stage, std::memory_order_release);
    }
    static elio::coro::task<int> discard_with_readonly_data(
        SparseRwLayer& layer, const std::string& path, uint64_t offset,
        uint64_t length) {
        const int readonly = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (readonly < 0) co_return -errno;
        struct Restore {
            int& descriptor;
            int original;
            int temporary;
            ~Restore() {
                descriptor = original;
                ::close(temporary);
            }
        } restore{layer.fd_, layer.fd_, readonly};
        // The kernel rejects reclamation through this read-only descriptor.
        // Metadata publication still uses the ordinary writable sidecars.
        layer.fd_ = readonly;
        co_return co_await layer.discard(offset, length);
    }
};
}  // namespace obd::format

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
    if (argc != 6 && argc != 7) return 64;
    const std::string lower = argc == 7 ? argv[6] : "";
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

    if (operation == "import-before-header")
        obd::format::SparseRwLayerTestAccess::interrupt_import_at(-5);
    if (operation == "import-after-header")
        obd::format::SparseRwLayerTestAccess::interrupt_import_at(-3);
    if (operation == "import-after-page")
        obd::format::SparseRwLayerTestAccess::interrupt_import_at(-4);
    if (operation == "init-before-data-growth")
        obd::format::SparseRwLayerTestAccess::interrupt_import_at(-6);
    if (operation == "init-after-data-growth")
        obd::format::SparseRwLayerTestAccess::interrupt_import_at(-7);
    try {
        elio::run_config runtime;
        runtime.num_threads = 1;
        runtime.blocking_threads = 1;
        return elio::run([path, operation, vsize, offset,
                          length, lower]() -> elio::coro::task<int> {
            if (operation.starts_with("extent-")) {
                if (operation.starts_with("extent-stage-"))
                    obd::format::SparseRwLayerTestAccess::extent_exit(std::stoi(operation.substr(13)));
                std::vector<std::unique_ptr<obd::format::LsmtLayer>> layers;
                if (!lower.empty()) {
                    auto local = co_await obd::source::LocalFileSource::open(lower);
                    obd::source::BlobSourcePtr blob = std::move(local);
                    layers.push_back(co_await obd::format::LsmtLayer::open(std::move(blob)));
                }
                auto top = co_await obd::format::SparseRwLayer::open(path, vsize);
                auto merged = co_await obd::format::MergedWritable::open(std::move(layers), std::move(top));
                if (operation == "extent-stress") {
                    const int progress = ::open((path + ".progress").c_str(),
                        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
                    if (progress < 0) ::_exit(65);
                    std::vector<uint8_t> bytes(512, 0xa5);
                    for (uint64_t block = 0; block < vsize / 4096; ++block) {
                        // Keep one block unissued if the parent is scheduled
                        // late; faster runs must not make the interruption
                        // oracle fail just because they finished the pass.
                        if (block + 1 == vsize / 4096) for (;;) ::pause();
                        const auto n = co_await merged->pwrite(bytes.data(), bytes.size(), block * 4096 + 512);
                        if (n != 512) ::_exit(65);
                        const uint64_t completed = block + 1;
                        if (::write(progress, &completed, sizeof(completed)) != sizeof(completed)) ::_exit(65);
                    }
                    // The parent owns the kill/reap even if this host completed
                    // the entire pass before its scheduled interruption.
                    for (;;) ::pause();
                }
                if (operation == "extent-discard") {
                    const int rc = co_await merged->discard(offset, length);
                    ::_exit(rc == 0 ? 0 : 65);
                }
                std::vector<uint8_t> data(static_cast<size_t>(length), 0xa5);
                const auto written = co_await merged->pwrite(data.data(), data.size(), offset);
                ::_exit(written == static_cast<ssize_t>(data.size()) ? 0 : 65);
            }
            auto layer =
                co_await obd::format::SparseRwLayer::open_legacy_for_test(path, vsize);
            int rc = -EINVAL;
            if (operation == "write" || operation == "write-zero") {
                std::vector<uint8_t> data(static_cast<size_t>(length),
                    operation == "write" ? 0xa5 : 0);
                const ssize_t written =
                    co_await layer->pwrite(data.data(), data.size(), offset);
                rc = written == static_cast<ssize_t>(data.size()) ? 0
                                                                  : -EIO;
            } else if (operation == "discard") {
                rc = co_await layer->discard(offset, length);
            } else if (operation == "discard-partial") {
                obd::format::SparseRwLayerTestAccess::fail_coverage_page(*layer, 1);
                rc = co_await layer->discard(offset, length);
                ::_exit(rc == -EIO ? 87 : 65);
            } else if (operation == "discard-readonly") {
                rc = co_await obd::format::SparseRwLayerTestAccess::
                    discard_with_readonly_data(*layer, path, offset, length);
            }
            ::_exit(rc == 0 ? 0 : 65);
        }, runtime);
    } catch (...) {
        return 66;
    }
}
