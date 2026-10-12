// DART proxy integration. See dart.hpp.
#include "source/dart.hpp"

#include <elio/coro/when_any.hpp>
#include <elio/net/resolve_wait.hpp>
#include <elio/net/tcp.hpp>
#include <elio/time/timer.hpp>

namespace obd::source {

namespace {
/// Reachability probe budget (overlaybd probes the accelerate URL with a
/// short timeout before deciding to use it).
constexpr std::chrono::milliseconds kReachableProbeTimeout{1000};
}  // namespace

std::optional<DartProxyAddress> parse_dart_address(std::string_view address) {
    // Strip an optional scheme; DART speaks plain HTTP regardless.
    if (auto p = address.find("://"); p != std::string_view::npos) {
        address.remove_prefix(p + 3);
    }
    DartProxyAddress out;
    const auto slash = address.find('/');
    const std::string_view authority =
        slash == std::string_view::npos ? address : address.substr(0, slash);
    out.prefix =
        slash == std::string_view::npos
            ? std::string("/dart")
            : std::string(address.substr(slash));

    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    out.host = std::string(authority.substr(0, colon));
    if (out.host.empty()) return std::nullopt;
    const std::string port_str(authority.substr(colon + 1));
    try {
        const unsigned long p = std::stoul(port_str);
        if (p == 0 || p > 65535) return std::nullopt;
        out.port = static_cast<uint16_t>(p);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    out.base = "http://" + out.host + ":" + std::to_string(out.port) +
               out.prefix;
    return out;
}

std::string dart_prefixed_url(const DartProxyAddress& address,
                              const std::string& actual_url) {
    return address.base + "/" + actual_url;
}

elio::coro::task<bool> dart_proxy_reachable(const DartProxyAddress& address) {
    // Bounded probe (overlaybd check_accelerate_url semantics): a silent
    // network (dropped SYNs) must fall back to direct reads quickly, not
    // stall image open behind TCP retransmits.
    const auto deadline = std::chrono::steady_clock::now() + kReachableProbeTimeout;
    auto probe = [&address, deadline](elio::coro::cancel_token token)
        -> elio::coro::task<bool> {
        try {
            elio::net::resolve_wait_options options;
            options.deadline = deadline;
            auto resolved = co_await elio::net::resolve_all(
                address.host, address.port, std::move(options), token);
            if (!resolved || std::chrono::steady_clock::now() >= deadline)
                co_return false;
            // Cancellable connect: the token fires at the probe deadline so
            // DNS and a silently-dropping TCP peer share the original budget.
            // Elio owns late libc lookup work after our DNS observer departs.
#ifdef OBD_TEST_HOOKS
            if (auto connect = detail::dart_probe_connect_for_test.load()) {
                const auto connected = co_await connect(std::move(token));
                co_return connected && std::chrono::steady_clock::now() < deadline;
            }
#endif
            auto stream = co_await elio::net::tcp_connect(resolved.addresses.front(),
                                                          std::move(token));
            co_return stream.has_value() && std::chrono::steady_clock::now() < deadline;
        } catch (const std::exception&) {
            co_return false;
        }
    };
    try {
        auto [winner, reachable] = co_await elio::when_any(
            std::move(probe),
            [deadline](elio::coro::cancel_token token) -> elio::coro::task<bool> {
                // Compute the remaining budget when this child actually runs;
                // a delayed timer child must not start a fresh one-second wait.
#ifdef OBD_TEST_HOOKS
                if (auto observe = detail::dart_probe_timer_deadline_for_test.load())
                    observe(deadline);
#endif
                co_await elio::time::sleep_for(
                    deadline - std::chrono::steady_clock::now(), std::move(token));
                co_return false;
            });
        co_return winner == 0 && reachable;
    } catch (const std::exception&) {
        co_return false;
    }
}

}  // namespace obd::source
