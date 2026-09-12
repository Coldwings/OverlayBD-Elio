#include "image/image_file.hpp"
#include "format/zfile.hpp"
#include "format/lsmt_format.hpp"
#include "source/local_file.hpp"
#include "../support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>

TEST_CASE("image: upstream TurboOCI gzip fixture reads original payload", "[image][turboci]") {
    using namespace obd;
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures/turboci";
    const auto rc = test::run_coro([&]() -> elio::coro::task<int> {
        auto local = co_await source::LocalFileSource::open((fixture / "ext4.fs.meta").string());
        auto meta = co_await format::ZFileSource::open(std::move(local), true);
        std::vector<uint8_t> trailer(4096);
        const auto trailer_read = co_await meta->pread(trailer.data(), trailer.size(), meta->size() - trailer.size());
        REQUIRE(trailer_read == 4096);
        const auto header = format::lsmt::HeaderTrailer::parse(trailer.data());
        std::vector<uint8_t> index(header.index_size * 16);
        const auto index_read = co_await meta->pread(index.data(), index.size(), header.index_offset);
        REQUIRE(index_read == static_cast<ssize_t>(index.size()));
        nlohmann::json j = {{"lowers", nlohmann::json::array({{
            {"file", (fixture / "ext4.fs.meta").string()},
            {"targetFile", (fixture / "original.tar.gz").string()},
            {"gzipIndex", (fixture / "gzip.meta").string()}}})}};
        const auto cfg = image::ImageConfig::from_json_text(j.dump(), {});
        image::GlobalConfig global;
        global.prefetch_enable = false;
        auto opened = co_await image::open_image(cfg, global);
        REQUIRE(opened.virtual_size == 1024ULL * 1024 * 1024);
        size_t checked = 0;
        for (uint64_t k = 0; k < header.index_size; ++k) {
            const auto m = bytes::load_segment_le(index.data() + k * 16);
            if (m.offset == bytes::segment_mapping::kInvalidOffset || m.tag != 1 || m.zeroed) continue;
            REQUIRE(m.moffset >= 1);
            std::vector<uint8_t> data(static_cast<size_t>(m.length) * 512);
            const auto n = co_await opened.root->pread(data.data(), data.size(), m.offset * 512);
            REQUIRE(n == static_cast<ssize_t>(data.size()));
            std::vector<uint8_t> expected_data(data.size());
            for (size_t b = 0; b < data.size(); ++b) {
                const uint64_t position = m.moffset * 512 - 512 + b;
                const uint8_t expected = position < 3 * 1024 * 1024 + 123 ?
                    static_cast<uint8_t>((position * 17 + position / 251) % 256) : 0;
                expected_data[b] = expected;
            }
            REQUIRE(data == expected_data);
            checked += data.size();
        }
        REQUIRE(checked >= 3 * 1024 * 1024);
        co_return 0;
    });
    REQUIRE(rc == 0);
}
