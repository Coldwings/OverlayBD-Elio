#include "format/hybrid_writer.hpp"
#include "format/hybrid_transaction.hpp"
#include "format/lsmt.hpp"
#include "source/local_file.hpp"
#include "common/errors.hpp"
#include "../support.hpp"
#include <elio/runtime/spawn_blocking.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <system_error>

using namespace obd;
namespace {
struct ResetFault {
    ~ResetFault() { format::hybrid::transaction_fault_for_test(-1); }
};
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
TEST_CASE("format: hybrid grow recovers both headers after publication failures",
          "[format][hybrid]") {
    for (const int stage : {-1, 1, 2, 3, 7}) {
        DYNAMIC_SECTION("stage=" << stage) {
            ResetFault reset;
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                std::vector<uint8_t> payload(512, 0x5a);
                const auto written = co_await writer->pwrite(payload.data(), payload.size(), 512);
                REQUIRE(written == 512);
                format::hybrid::transaction_fault_for_test(stage);
                auto* active = writer.get();
                const int grown = co_await elio::spawn_blocking([active] { return active->grow(65536); });
                REQUIRE(grown == (stage == -1 ? 0 : -EIO));
                format::hybrid::transaction_fault_for_test(-1);
                writer.reset();
                pair = co_await format::hybrid::FilePair::open_existing(
                    directory + "/data", directory + "/index", true);
                writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                REQUIRE(writer->virtual_size() == 65536);
                std::vector<uint8_t> actual(512);
                const auto read = co_await writer->pread(actual.data(), actual.size(), 512);
                REQUIRE(read == 512);
                REQUIRE(actual == payload);
                const auto last = co_await writer->pwrite(payload.data(), payload.size(), 65536 - 512);
                REQUIRE(last == 512);
                const int flushed = co_await writer->flush();
                REQUIRE(flushed == 0);
                co_return 0;
            });
            REQUIRE(rc == 0);
            REQUIRE_FALSE(std::filesystem::exists(directory + "/index.txn"));
        }
    }
}

TEST_CASE("format: hybrid seal recovery appends metadata without moving payload",
          "[format][hybrid]") {
    for (const int stage : {-1, 1, 4, 5, 6, 7}) {
        DYNAMIC_SECTION("stage=" << stage) {
            ResetFault reset;
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            std::vector<uint8_t> original;
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                std::vector<uint8_t> payload(512, 0x5a);
                const auto written = co_await writer->pwrite(payload.data(), payload.size(), 512);
                REQUIRE(written == 512);
                original = read_file(directory + "/data");
                format::hybrid::transaction_fault_for_test(stage);
                const int sealed = co_await writer->seal("transaction test");
                REQUIRE(sealed == (stage == -1 ? 0 : -EIO));
                format::hybrid::transaction_fault_for_test(-1);
                writer.reset();
                int refusal = 0;
                try {
                    pair = co_await format::hybrid::FilePair::open_existing(
                        directory + "/data", directory + "/index", true);
                } catch (const error& e) { refusal = e.errno_value(); }
                REQUIRE(refusal == EALREADY);
                auto local = co_await source::LocalFileSource::open(directory + "/data");
                auto layer = co_await format::LsmtLayer::open(std::move(local));
                REQUIRE(layer->header().user_tag == "transaction test");
                std::vector<std::unique_ptr<format::LsmtLayer>> layers;
                layers.push_back(std::move(layer));
                auto merged = co_await format::MergedLsmt::open(std::move(layers));
                std::vector<uint8_t> actual(32768), expected(32768, 0);
                std::fill_n(expected.begin() + 512, 512, 0x5a);
                const auto read = co_await merged->pread(actual.data(), actual.size(), 0);
                REQUIRE(read == 32768);
                REQUIRE(actual == expected);
                co_return 0;
            });
            REQUIRE(rc == 0);
            const auto sealed = read_file(directory + "/data");
            REQUIRE(sealed.size() == 12288);
            REQUIRE(std::equal(original.begin(), original.end(), sealed.begin()));
            REQUIRE_FALSE(std::filesystem::exists(directory + "/index.txn"));
        }
    }
}

namespace {
int run_fixture(std::initializer_list<std::string> arguments) {
    std::vector<std::string> owned{OBD_TEST_HYBRID_WRITER_BIN};
    owned.insert(owned.end(), arguments);
    std::vector<char*> argv;
    for (auto& argument : owned) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child < 0) throw std::system_error(errno, std::system_category(), "fork");
    if (child == 0) {
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::system_error(errno, std::system_category(), "waitpid");
    }
    REQUIRE(WIFEXITED(status));
    return WEXITSTATUS(status);
}
void replace_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream.good());
}
std::vector<uint8_t> fixture_bytes(size_t size) {
    std::vector<uint8_t> expected(size);
    for (const auto& [sector, value] : {std::pair{1, 0x11}, {2, 0x33},
                                     {8, 0x22}, {9, 0x44}, {63, 0x77}})
        std::fill_n(expected.begin() + sector * 512, 512, static_cast<uint8_t>(value));
    return expected;
}
}

TEST_CASE("format: hybrid publication survives independent process exit",
          "[format][hybrid]") {
    for (const bool grow : {true, false}) {
        const std::vector<int> stages = grow ? std::vector<int>{1, 2, 3, 7}
                                            : std::vector<int>{1, 4, 5, 6, 7};
        for (const int stage : stages) {
            DYNAMIC_SECTION("grow=" << grow << " stage=" << stage) {
                test::TempDir temporary;
                const auto directory = temporary / "pair";
                const auto data = directory + "/data", index = directory + "/index";
                REQUIRE(run_fixture({directory}) == 0);
                const auto original_data = read_file(data), original_index = read_file(index);
                REQUIRE(run_fixture({grow ? "crash-grow" : "crash-seal", data, index,
                                     std::to_string(stage)}) == 86);
                REQUIRE(run_fixture({"recover", data, index}) == 0);
                REQUIRE_FALSE(std::filesystem::exists(index + ".txn"));
                const auto recovered_data = read_file(data), recovered_index = read_file(index);
                REQUIRE(run_fixture({"recover", data, index}) == 0);
                REQUIRE(read_file(data) == recovered_data);
                REQUIRE(read_file(index) == recovered_index);
                if (grow) {
                    REQUIRE(recovered_data.size() == original_data.size());
                    REQUIRE(recovered_index.size() == original_index.size());
                    REQUIRE(std::equal(original_data.begin() + 4096, original_data.end(),
                                       recovered_data.begin() + 4096));
                    REQUIRE(std::equal(original_index.begin() + 4096, original_index.end(),
                                       recovered_index.begin() + 4096));
                } else {
                    REQUIRE(recovered_data.size() > original_data.size());
                    REQUIRE(std::equal(original_data.begin(), original_data.end(), recovered_data.begin()));
                }
                const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                    const auto expected = fixture_bytes(grow ? 65536 : 32768);
                    std::vector<uint8_t> actual(expected.size());
                    ssize_t read = 0;
                    if (grow) {
                        auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                        format::hybrid::Writer writer(std::move(pair));
                        REQUIRE(writer.virtual_size() == expected.size());
                        read = co_await writer.pread(actual.data(), actual.size(), 0);
                    } else {
                        auto source = co_await source::LocalFileSource::open(data);
                        auto layer = co_await format::LsmtLayer::open(std::move(source));
                        std::vector<std::unique_ptr<format::LsmtLayer>> layers;
                        layers.push_back(std::move(layer));
                        auto merged = co_await format::MergedLsmt::open(std::move(layers));
                        read = co_await merged->pread(actual.data(), actual.size(), 0);
                    }
                    REQUIRE(read == static_cast<ssize_t>(expected.size()));
                    REQUIRE(actual == expected);
                    co_return 0;
                });
                REQUIRE(rc == 0);
            }
        }
    }
}

TEST_CASE("format: hybrid recovery refuses damaged or stale transaction intent",
          "[format][hybrid]") {
    for (const std::string fault : {"checksum", "truncated", "inode", "index-content"}) {
        DYNAMIC_SECTION(fault) {
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const auto data = directory + "/data", index = directory + "/index";
            const auto intent = index + ".txn";
            REQUIRE(run_fixture({directory}) == 0);
            REQUIRE(run_fixture({"crash-grow", data, index, "1"}) == 86);
            const auto original_data = read_file(data), original_index = read_file(index);
            const auto original_intent = read_file(intent);
            if (fault == "inode") {
                std::filesystem::create_hard_link(index, index + ".saved");
                replace_bytes(index + ".replacement", original_index);
                std::filesystem::rename(index + ".replacement", index);
            } else if (fault == "index-content") {
                auto changed = original_index;
                changed.back() ^= 1;
                replace_bytes(index, changed);
            } else {
                auto changed = original_intent;
                if (fault == "truncated") changed.resize(100);
                else changed[100] ^= 1;
                replace_bytes(intent, changed);
            }
            const auto before_index = read_file(index);
            REQUIRE(run_fixture({"recover", data, index}) == 1);
            REQUIRE(read_file(data) == original_data);
            REQUIRE(read_file(index) == before_index);
            REQUIRE(std::filesystem::exists(intent));
            if (fault == "inode") std::filesystem::rename(index + ".saved", index);
            else if (fault == "index-content") replace_bytes(index, original_index);
            else replace_bytes(intent, original_intent);
            REQUIRE(run_fixture({"recover", data, index}) == 0);
            REQUIRE_FALSE(std::filesystem::exists(intent));
        }
    }
}

TEST_CASE("format: hybrid completed unflushed operations survive process exit",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    REQUIRE(run_fixture({"crash-unflushed", directory}) == 87);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto pair = co_await format::hybrid::FilePair::open_existing(
            directory + "/data", directory + "/index", true);
        format::hybrid::Writer writer(std::move(pair));
        std::vector<uint8_t> actual(8192), expected(8192);
        std::fill(expected.begin(), expected.begin() + 2048, 0x31);
        std::fill(expected.begin() + 512, expected.begin() + 1024, 0x55);
        std::fill(expected.begin() + 1024, expected.begin() + 1536, 0);
        const auto read = co_await writer.pread(actual.data(), actual.size(), 0);
        REQUIRE(read == static_cast<ssize_t>(actual.size()));
        REQUIRE(actual == expected);
        const auto mappings = writer.snapshot();
        for (const uint64_t sector : {2u, 8u}) {
            const auto found = std::find_if(mappings.begin(), mappings.end(),
                [sector](const auto& m) { return m.offset <= sector && sector < m.end(); });
            REQUIRE(found != mappings.end());
            REQUIRE(found->zeroed);
        }
        std::vector<uint8_t> continued(512, 0x77);
        const auto written = co_await writer.pwrite(continued.data(), continued.size(), 8192);
        REQUIRE(written == 512);
        const auto reread = co_await writer.pread(actual.data(), continued.size(), 8192);
        REQUIRE(reread == 512);
        REQUIRE(std::equal(continued.begin(), continued.end(), actual.begin()));
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("format: hybrid trailer shaped guest payload remains writable",
          "[format][hybrid]") {
    for (const std::string action : {"reopen", "grow", "seal", "discard"}) {
        DYNAMIC_SECTION(action) {
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                // Matching trailer fields alone are insufficient: this payload
                // leaves no room for its declared index before the trailer.
                auto trailer = pair->header();
                trailer.flags = format::hybrid::kHybridFlag | 6u;
                trailer.index_offset = 4096;
                trailer.index_size = 1;
                std::vector<uint8_t> payload(4096);
                trailer.serialize(payload.data());
                auto writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                const auto written = co_await writer->pwrite(payload.data(), payload.size(), 0);
                REQUIRE(written == 4096);
                if (action == "discard") {
                    const int discarded = co_await writer->discard(0, payload.size());
                    REQUIRE(discarded == 0);
                    std::fill(payload.begin(), payload.end(), 0);
                }
                const int flushed = co_await writer->flush();
                REQUIRE(flushed == 0);
                if (action == "reopen" || action == "discard") {
                    writer.reset();
                    pair = co_await format::hybrid::FilePair::open_existing(
                        directory + "/data", directory + "/index", true);
                    writer = std::make_unique<format::hybrid::Writer>(std::move(pair));
                } else if (action == "grow") {
                    auto* active = writer.get();
                    const int grown = co_await elio::spawn_blocking([active] { return active->grow(65536); });
                    REQUIRE(grown == 0);
                } else {
                    const int sealed = co_await writer->seal("guest trailer");
                    REQUIRE(sealed == 0);
                    writer.reset();
                    auto local = co_await source::LocalFileSource::open(directory + "/data");
                    std::vector<std::unique_ptr<format::LsmtLayer>> layers;
                    layers.push_back(co_await format::LsmtLayer::open(std::move(local)));
                    auto merged = co_await format::MergedLsmt::open(std::move(layers));
                    std::vector<uint8_t> actual(payload.size());
                    const auto read = co_await merged->pread(actual.data(), actual.size(), 0);
                    REQUIRE(read == 4096);
                    REQUIRE(actual == payload);
                    co_return 0;
                }
                std::vector<uint8_t> actual(payload.size());
                const auto read = co_await writer->pread(actual.data(), actual.size(), 0);
                REQUIRE(read == 4096);
                REQUIRE(actual == payload);
                co_return 0;
            });
            REQUIRE(rc == 0);
        }
    }
}

TEST_CASE("format: hybrid sealed EOF ZERO marker never becomes writable metadata",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "fixtures/hybrid/agentenv/writable.data";
    auto header_bytes = read_file(fixtures.string());
    auto header = format::lsmt::HeaderTrailer::parse(header_bytes.data());
    std::vector<uint8_t> data(12288), index(4112);
    header.serialize(data.data());
    auto index_header = header;
    index_header.flags = format::hybrid::kHybridFlag | 1u;
    index_header.index_offset = 4096;
    index_header.serialize(index.data());
    bytes::segment_mapping zero{};
    zero.offset = 0;
    zero.length = 1;
    zero.moffset = 8; // Original EOF, not backing allocated by the appended index.
    zero.zeroed = true;
    bytes::store_segment_le(index.data() + 4096, zero);
    bytes::store_segment_le(data.data() + 4096, zero);
    header.flags = format::hybrid::kHybridFlag | 6u;
    header.index_offset = 4096;
    header.index_size = 1;
    header.serialize(data.data() + 8192);
    const auto data_path = test::write_file(temporary / "data", data);
    const auto index_path = test::write_file(temporary / "index", index);
    const int rc = test::run_coro([&]() -> elio::coro::task<int> {
        int refusal = 0;
        try {
            auto pair = co_await format::hybrid::FilePair::open_existing(data_path, index_path, true);
            (void)pair;
        } catch (const error& error) { refusal = error.errno_value(); }
        REQUIRE(refusal == EALREADY);
        co_return 0;
    });
    REQUIRE(rc == 0);
    REQUIRE(read_file(data_path) == data);
    REQUIRE(read_file(index_path) == index);
}

TEST_CASE("format: hybrid upstream sealed index alignments refuse writable reopen",
          "[format][hybrid]") {
    // OverlayBD close_seal pads the compact index to 512; AgentENV pads
    // the trailer position to 4096. Additional padding is also accepted by
    // the sealed reader and must not turn the file into a writable pair.
    for (const size_t tail_offset : {5120u, 8192u, 12288u}) {
        DYNAMIC_SECTION(tail_offset) {
            test::TempDir temporary;
            const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                                 "fixtures/hybrid/overlaybd/writable.data";
            const auto original = read_file(fixture.string());
            auto header = format::lsmt::HeaderTrailer::parse(original.data());
            std::vector<uint8_t> data(tail_offset + 4096), index(4112);
            header.serialize(data.data());
            std::fill(data.begin() + 4096, data.begin() + 4608, 0x5a);
            auto sidecar = header;
            sidecar.flags = format::hybrid::kHybridFlag | 1u;
            sidecar.index_offset = 4096;
            sidecar.serialize(index.data());
            bytes::segment_mapping mapping{};
            mapping.length = 1;
            mapping.moffset = 8;
            bytes::store_segment_le(index.data() + 4096, mapping);
            bytes::store_segment_le(data.data() + 4608, mapping);
            header.flags = format::hybrid::kHybridFlag | 6u;
            header.index_offset = 4608;
            header.index_size = 1;
            if (tail_offset == 12288) {
                // The sealed reader uses trailer capacity and identity.
                header.virtual_size *= 2;
                header.uuid = "12345678-1234-4234-8234-123456789abc";
                header.reserved = 1;
            }
            header.serialize(data.data() + tail_offset);
            const auto data_path = test::write_file(temporary / "data", data);
            const auto index_path = test::write_file(temporary / "index", index);
            const int rc = test::run_coro([&]() -> elio::coro::task<int> {
                int refusal = 0;
                try {
                    auto pair = co_await format::hybrid::FilePair::open_existing(
                        data_path, index_path, true);
                    (void)pair;
                } catch (const error& error) { refusal = error.errno_value(); }
                REQUIRE(refusal == EALREADY);
                auto local = co_await source::LocalFileSource::open(data_path);
                auto lower = co_await format::LsmtLayer::open(std::move(local));
                REQUIRE(lower->virtual_size() == header.virtual_size);
                co_return 0;
            });
            REQUIRE(rc == 0);
            REQUIRE(read_file(data_path) == data);
            REQUIRE(read_file(index_path) == index);
        }
    }
}

TEST_CASE("format: hybrid complete trailer overrides local attributes", "[format][hybrid]") {
    for (const std::string attributes : {"native", "stale-marker", "absent"}) {
        DYNAMIC_SECTION(attributes) {
            test::TempDir temporary;
            const auto directory = temporary / "pair";
            const int run_result_1 = test::run_coro([&]() -> elio::coro::task<int> {
                auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
                format::hybrid::Writer writer(std::move(pair));
                co_return co_await writer.seal("trailer state");
            });
            REQUIRE(run_result_1 == 0);
            const auto data = directory + "/data";
            REQUIRE(::getxattr(data.c_str(), "user.overlaybd.hybrid.sealed", nullptr, 0) == -1);
            REQUIRE(errno == ENODATA);
            if (attributes == "stale-marker") {
                const char stale[] = "not a seal state";
                REQUIRE(::setxattr(data.c_str(), "user.overlaybd.hybrid.sealed", stale, sizeof(stale), 0) == 0);
            } else if (attributes == "absent") {
                REQUIRE(::removexattr(data.c_str(), "user.overlaybd.hybrid.log") == 0);
                REQUIRE(::removexattr((directory + "/index").c_str(), "user.overlaybd.hybrid.log") == 0);
            }
            const int run_result_2 = test::run_coro([&]() -> elio::coro::task<int> {
                int refusal = 0;
                try {
                    auto pair = co_await format::hybrid::FilePair::open_existing(data, directory + "/index", true);
                    (void)pair;
                } catch (const error& error) { refusal = error.errno_value(); }
                REQUIRE(refusal == EALREADY);
                auto local = co_await source::LocalFileSource::open(data);
                auto lower = co_await format::LsmtLayer::open(std::move(local));
                REQUIRE(lower->virtual_size() == 32768);
                co_return 0;
            });
            REQUIRE(run_result_2 == 0);
        }
    }
}

TEST_CASE("format: hybrid missing trailer is not a sealed lower regardless of attributes",
          "[format][hybrid]") {
    for (const bool stale_marker : {false, true}) {
        test::TempDir temporary;
        const auto directory = temporary / "pair";
        const int run_result_3 = test::run_coro([&]() -> elio::coro::task<int> {
            auto pair = co_await format::hybrid::FilePair::create(directory, 32768);
            pair.reset();
            const auto data = directory + "/data";
            if (stale_marker) {
                const char stale[] = "sealed";
                REQUIRE(::setxattr(data.c_str(), "user.overlaybd.hybrid.sealed", stale, sizeof(stale), 0) == 0);
            } else {
                REQUIRE(::removexattr(data.c_str(), "user.overlaybd.hybrid.log") == 0);
                REQUIRE(::removexattr((directory + "/index").c_str(), "user.overlaybd.hybrid.log") == 0);
            }
            auto local = co_await source::LocalFileSource::open(data);
            bool rejected = false;
            try { auto lower = co_await format::LsmtLayer::open(std::move(local)); (void)lower; }
            catch (const format_error&) { rejected = true; }
            REQUIRE(rejected);
            pair = co_await format::hybrid::FilePair::open_existing(data, directory + "/index", true);
            REQUIRE(pair->writable());
            co_return 0;
        });
        REQUIRE(run_result_3 == 0);
    }
}

TEST_CASE("format: hybrid complete payload trailer is sealed with or without local attributes",
          "[format][hybrid]") {
    test::TempDir temporary;
    const auto directory = temporary / "pair";
    REQUIRE(run_fixture({"unsealed-trailer-payload", directory}) == 0);
    const auto data = directory + "/data";
    const auto index = directory + "/index";
    const auto before_data = read_file(data);
    const auto before_index = read_file(index);
    for (const bool remove_attributes : {false, true}) {
        if (remove_attributes) {
            REQUIRE(::removexattr(data.c_str(), "user.overlaybd.hybrid.log") == 0);
            REQUIRE(::removexattr(index.c_str(), "user.overlaybd.hybrid.log") == 0);
        }
        const int run_result_4 = test::run_coro([&]() -> elio::coro::task<int> {
            int refusal = 0;
            try {
                auto pair = co_await format::hybrid::FilePair::open_existing(data, index, true);
                (void)pair;
            } catch (const error& error) { refusal = error.errno_value(); }
            REQUIRE(refusal == EALREADY);
            auto local = co_await source::LocalFileSource::open(data);
            auto lower = co_await format::LsmtLayer::open(std::move(local));
            REQUIRE(lower->virtual_size() == 32768);
            co_return 0;
        });
        REQUIRE(run_result_4 == 0);
        REQUIRE(read_file(data) == before_data);
        REQUIRE(read_file(index) == before_index);
    }
}
