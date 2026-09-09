// Unit tests: source module — tar adapter, credentials, DART address
// handling.
#include "source/credentials.hpp"
#include "source/dart.hpp"
#include "source/tar_offset.hpp"

#include "../support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace obd;
using obd::test::VectorSource;

TEST_CASE("source: tar adapter detects ustar wrapper and skips the header",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        // blob = tar header (payload 1000) + 1000 payload + padding.
        auto payload = test::pattern_bytes(1000, 5);
        std::vector<uint8_t> blob;
        auto hdr = test::make_tar_header(1000);
        blob.insert(blob.end(), hdr.begin(), hdr.end());
        blob.insert(blob.end(), payload.begin(), payload.end());
        blob.resize(512 + 1536, 0);  // trailing padding like real blobs

        source::BlobSourcePtr base =
            std::make_unique<VectorSource>(std::move(blob));
        auto adapted = co_await source::TarOffsetSource::open(std::move(base));
        auto* tar = dynamic_cast<source::TarOffsetSource*>(adapted.get());
        REQUIRE(tar != nullptr);
        REQUIRE(tar->base_offset() == 512);
        REQUIRE(tar->size() == 1000);
        std::vector<uint8_t> buf(1000);
        const ssize_t r = co_await adapted->pread(buf.data(), 1000, 0);
        REQUIRE(r == 1000);
        REQUIRE(buf == payload);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: tar adapter passes plain files through unwrapped",
          "[source]") {
    const int rc = test::run_coro([]() -> elio::coro::task<int> {
        auto payload = test::pattern_bytes(4096, 6);  // no ustar magic
        auto* vec = new VectorSource(payload);
        source::BlobSourcePtr base(vec);
        auto adapted = co_await source::TarOffsetSource::open(std::move(base));
        REQUIRE(dynamic_cast<source::TarOffsetSource*>(adapted.get()) ==
                nullptr);
        REQUIRE(adapted.get() == vec);
        REQUIRE(adapted->size() == 4096);
        co_return 0;
    });
    REQUIRE(rc == 0);
}

TEST_CASE("source: credential store longest-prefix matching", "[source]") {
    test::TempDir dir;
    const std::string cred_path = dir / "cred.json";
    const std::string json = R"({"auths":{
        "reg.example.com/lib": {"username":"u2","password":"p2"},
        "reg.example.com": {"username":"u1","password":"p1"},
        "other.example.com": {"auth":"bzpw"}
    }})";
    test::write_file(cred_path,
                     std::vector<uint8_t>(json.begin(), json.end()));
    auto store = source::CredentialStore::from_file(cred_path);
    REQUIRE(store.size() == 3);
    const auto c1 = store.find("https://reg.example.com/v2/x/blobs/sha256:a");
    REQUIRE(c1.has_value());
    REQUIRE(c1->username == "u1");
    const auto c2 = store.find("https://reg.example.com/lib/nginx/blobs/sha256:b");
    REQUIRE(c2.has_value());
    REQUIRE(c2->username == "u2");  // longest prefix wins
    const auto c3 = store.find("https://other.example.com/v2/x");
    REQUIRE(c3.has_value());
    REQUIRE(c3->username == "o");
    REQUIRE(c3->password == "p");
    REQUIRE(!store.find("https://unknown.example.com/v2/x").has_value());
}

TEST_CASE("source: DART address parsing and prefixed URL", "[source]") {
    const auto a = source::parse_dart_address("localhost:19145/dart");
    REQUIRE(a.has_value());
    REQUIRE(a->host == "localhost");
    REQUIRE(a->port == 19145);
    REQUIRE(a->prefix == "/dart");
    REQUIRE(a->base == "http://localhost:19145/dart");
    REQUIRE(source::dart_prefixed_url(*a, "https://reg.example.com/v2/x") ==
            "http://localhost:19145/dart/https://reg.example.com/v2/x");

    const auto b = source::parse_dart_address("http://127.0.0.1:9000");
    REQUIRE(b.has_value());
    REQUIRE(b->prefix == "/dart");  // default prefix
    REQUIRE(!source::parse_dart_address("localhost/dart").has_value());
    REQUIRE(!source::parse_dart_address("localhost:notaport/dart").has_value());
    REQUIRE(!source::parse_dart_address("localhost:99999/dart").has_value());
}
