// DART's real implementation, compiled in a dedicated executable with Elio's
// test-only DNS hook. Production uses only public resolve_wait APIs.
#include "source/dart.hpp"
#include "../support.hpp"

#include <elio/net/resolve_wait.hpp>
#include <elio/net/tcp.hpp>
#include <elio/time/timer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>

namespace {

struct LookupControl {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::atomic<size_t> calls{0};
    std::atomic<int64_t> deadline_ns{0};
    std::string late_host;
    int error = 0;
    bool throw_error = false;
};

std::atomic<LookupControl*> lookup_control{nullptr};

elio::net::detail::dns_lookup_result controlled_lookup(std::string_view host,
                                                       uint16_t port) {
    auto* control = lookup_control.load(std::memory_order_acquire);
    if (!control) throw std::logic_error("missing DNS lookup control");
    ++control->calls;
    control->entered.store(true, std::memory_order_release);
    while (!control->release.load(std::memory_order_acquire))
        control->release.wait(false, std::memory_order_acquire);
    // Read the producer's hostname only after the calling probe/address frame
    // has gone away. This detects borrowing across the observer's deadline.
    control->late_host = std::string(host);
    control->finished.store(true, std::memory_order_release);
    if (control->throw_error) throw std::runtime_error("controlled DNS exception");
    if (control->error) return {{}, control->error};
    return {{elio::net::socket_address(elio::net::ipv4_address("127.0.0.1", port))}, 0};
}

void release_lookup(LookupControl& control) {
    control.release.store(true, std::memory_order_release);
    control.release.notify_all();
}

elio::coro::task<elio::coro::cancel_result> observe_deadline(
    std::chrono::steady_clock::time_point deadline,
    elio::coro::cancel_token token) {
    auto* control = lookup_control.load(std::memory_order_acquire);
    if (!control) throw std::logic_error("missing DNS deadline control");
    control->deadline_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline.time_since_epoch()).count(), std::memory_order_release);
    co_return co_await elio::time::sleep_for(deadline - std::chrono::steady_clock::now(), token);
}

struct ReleaseLookup {
    LookupControl& control;
    ~ReleaseLookup() { release_lookup(control); }
};

struct LookupHook {
    explicit LookupHook(LookupControl& control) : control(control) {
        lookup_control.store(&control, std::memory_order_release);
        elio::net::detail::owned_dns_lookup_for_test.store(
            controlled_lookup, std::memory_order_release);
        elio::coro::detail::join_timer_wait_for_test.store(
            observe_deadline, std::memory_order_release);
    }
    ~LookupHook() {
        release_lookup(control);
        elio::net::detail::owned_dns_lookup_for_test.store(nullptr, std::memory_order_release);
        elio::coro::detail::join_timer_wait_for_test.store(nullptr, std::memory_order_release);
        lookup_control.store(nullptr, std::memory_order_release);
    }
    LookupControl& control;
};

elio::coro::task<void> wait_dns_retired() {
    const auto domain = elio::net::default_resolve_domain();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (domain->outstanding() != 0 && std::chrono::steady_clock::now() < deadline)
        co_await elio::time::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(domain->outstanding() == 0);
}

}  // namespace

TEST_CASE("source: DART DNS deadline leaves late lookup owned until retirement", "[source][dart-dns]") {
    LookupControl control;
    LookupHook hook(control);
    obd::test::run_coro([&]() -> elio::coro::task<void> {
        ReleaseLookup release{control};  // also releases producers on assertion failure
        bool reachable;
        const auto start = std::chrono::steady_clock::now();
        {
            obd::source::DartProxyAddress address;
            address.host = "owned-dart-probe.example";
            address.port = 19145;
            reachable = co_await obd::source::dart_proxy_reachable(address);
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE_FALSE(reachable);
        REQUIRE(control.entered.load(std::memory_order_acquire));
        REQUIRE_FALSE(control.finished.load(std::memory_order_acquire));
        REQUIRE(elapsed >= std::chrono::milliseconds(750));
        const auto deadline_ns = control.deadline_ns.load(std::memory_order_acquire);
        const auto start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            start.time_since_epoch()).count();
        REQUIRE(deadline_ns >= start_ns + 1000000000);
        // Scheduler preemption can delay execution or observation arbitrarily;
        // assert the registered deadline expired, not a physical return ceiling.
        const auto observed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        REQUIRE(deadline_ns <= observed_ns);
        // Observer timeout does not release the producer's admission lease.
        REQUIRE(elio::net::default_resolve_domain()->outstanding() == 1);
        release_lookup(control);
        co_await wait_dns_retired();
        REQUIRE(control.finished.load(std::memory_order_acquire));
        REQUIRE(control.late_host == "owned-dart-probe.example");
        REQUIRE(control.calls.load() == 1);
    });
}

TEST_CASE("source: DART DNS failure and exception fall back to direct reads", "[source][dart-dns]") {
    const bool exception = GENERATE(false, true);
    LookupControl control;
    control.release.store(true);
    control.error = EHOSTUNREACH;
    control.throw_error = exception;
    LookupHook hook(control);
    obd::test::run_coro([&]() -> elio::coro::task<void> {
        ReleaseLookup release{control};
        obd::source::DartProxyAddress address;
        address.host = "failing-dart-probe.example";
        address.port = 19145;
        const auto reachable = co_await obd::source::dart_proxy_reachable(address);
        REQUIRE_FALSE(reachable);
        co_await wait_dns_retired();
        REQUIRE(control.calls.load() == 1);
    });
}

TEST_CASE("source: DART literal probes bypass DNS and report TCP reachability", "[source][dart-dns]") {
    LookupControl control;
    control.release.store(true);
    LookupHook hook(control);
    obd::test::run_coro([&]() -> elio::coro::task<void> {
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        obd::source::DartProxyAddress address;
        address.host = "127.0.0.1";
        address.port = listener->local_address().port();
        const auto connected = co_await obd::source::dart_proxy_reachable(address);
        REQUIRE(connected);
        obd::test::ReservedTcpPort unlistened;
        address.port = unlistened.port();
        const auto refused = co_await obd::source::dart_proxy_reachable(address);
        REQUIRE_FALSE(refused);
        REQUIRE(control.calls.load() == 0);
        REQUIRE(elio::net::default_resolve_domain()->outstanding() == 0);
    });
}
