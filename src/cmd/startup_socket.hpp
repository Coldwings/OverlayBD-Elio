#pragma once

#include "supervisor/protocol.hpp"
#include "ublk/startup_control.hpp"

#include <elio/coro/task_group.hpp>
#include <elio/coro/with_timeout.hpp>
#include <elio/net/uds.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>

namespace obd::cmd {

inline obd::supervisor::StartupOwnerStatus startup_status(
    const obd::ublk::StartupSnapshot& snapshot) {
    obd::supervisor::StartupOwnerStatus status;
    status.generation = snapshot.generation;
    status.revision = snapshot.version;
    switch (snapshot.phase) {
    case obd::ublk::StartupPhase::preparing: status.phase = "preparing"; break;
    case obd::ublk::StartupPhase::control_pending: status.phase = "control_pending"; break;
    case obd::ublk::StartupPhase::retained: status.phase = "retained"; break;
    case obd::ublk::StartupPhase::releasing: status.phase = "releasing"; break;
    case obd::ublk::StartupPhase::ready: status.phase = "ready"; break;
    case obd::ublk::StartupPhase::released: status.phase = "released"; break;
    }
    switch (snapshot.release) {
    case obd::ublk::StartupRelease::none: status.release = "none"; break;
    case obd::ublk::StartupRelease::pending: status.release = "pending"; break;
    case obd::ublk::StartupRelease::refused: status.release = "refused"; break;
    case obd::ublk::StartupRelease::released: status.release = "released"; break;
    }
    status.device_id = snapshot.device_id == UINT32_MAX
        ? -1 : static_cast<int>(snapshot.device_id);
    status.reason_errno = snapshot.reason_errno;
    status.control_pending = snapshot.control_pending;
    status.kernel_debt_known = snapshot.kernel_debt_known;
    status.raw_published = snapshot.raw_published;
    status.raw_retired = snapshot.raw_retired;
    status.release_sequence = snapshot.release_sequence;
    status.release_errno = snapshot.release_errno;
    return status;
}

/// Standalone startup management. The caller owns this object and StartupControl
/// until run() and its native frame have drained. finish() is legal only after
/// all device/image owners have retired; it closes admission and supplies the
/// terminal report to an accepted release client before the socket disappears.
class StartupSocket {
public:
    StartupSocket(std::string name, ublk::StartupControl& startup)
        : startup_(startup), listener_(bind(name)) {}

    static bool valid_name(const std::string& name) noexcept {
        return name.size() > 1 && name.front() == '@' &&
               name.find('\0') == std::string::npos &&
               name.size() <= sizeof(sockaddr_un::sun_path);
    }

    bool started() const noexcept { return started_.load(std::memory_order_acquire); }

    void finish(const ublk::StartupSnapshot& terminal) noexcept {
        if (finished_.load(std::memory_order_acquire)) return;
        terminal_ = terminal;
        finished_.store(true, std::memory_order_release);
    }

    elio::coro::task<void> run() {
        // Allocate all aggregate ownership before admitting connections. Each
        // handler owns its stream, and a slow peer cannot occupy the acceptor.
        elio::coro::task_group clients({
            .failure_policy = elio::coro::task_group_failure_policy::collect_all});
        started_.store(true, std::memory_order_release);
        while (!finished_.load(std::memory_order_acquire)) {
            try {
                // An idle listener observes finish within a bounded interval.
                // No one-shot external cancellation can strand this acceptor
                // if a cancellation observer throws during finalization.
                auto accepted = co_await elio::with_timeout(std::chrono::milliseconds(100),
                    [&](elio::coro::cancel_token token)
                        -> elio::coro::task<std::optional<elio::net::uds_stream>> {
                        co_return co_await listener_.accept(std::move(token));
                    });
                if (!accepted || !*accepted ||
                    finished_.load(std::memory_order_acquire)) continue;
                auto stream = std::move(*accepted);
                ucred peer{};
                socklen_t size = sizeof(peer);
                if (::getsockopt(stream->fd(), SOL_SOCKET, SO_PEERCRED,
                                 &peer, &size) != 0 || size != sizeof(peer) ||
                    (peer.uid != ::geteuid() && peer.uid != 0)) continue;
                // Bounds untrusted partial requests while reserving capacity
                // for concurrent status/release commands from an operator.
                if (clients.outstanding_children() >= 32) continue;
                clients.spawn([this, stream = std::move(*stream)]() mutable
                                  -> elio::coro::task<void> {
                    try { co_await handle(stream); } catch (...) {}
                });
            } catch (...) {
                // Allocation/admission errors discard only this connection;
                // the retained owner must keep its operational release path.
            }
            if (!finished_.load(std::memory_order_acquire)) {
                try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
                catch (...) {}
            }
        }
        // No IO may still borrow this listener when its fd is closed.
        listener_.close();
        // Keep the single-use join observer alive across any setup failure.
        // Its fallback proof is the TaskGroup native-frame counter, which is
        // decremented only after child callable/argument capture destruction.
        auto joined = clients.join();
        try {
#ifdef OBD_TEST_HOOKS
            if (before_join_) before_join_(clients.outstanding_children(), join_context_);
#endif
            co_await joined;
        } catch (...) {}
        while (clients.outstanding_children() != 0) {
            try { co_await elio::time::sleep_for(std::chrono::milliseconds(1)); }
            catch (...) {}
        }
    }

private:
    static elio::net::uds_listener bind(const std::string& name) {
        if (!valid_name(name))
            throw std::invalid_argument("--startup-socket requires @NAME (at most 107 name bytes)");
        elio::net::uds_options options;
        options.unlink_on_bind = false;
        auto listener = elio::net::uds_listener::bind(
            elio::net::unix_address::abstract(std::string_view(name).substr(1)), options);
        if (!listener)
            throw std::system_error(errno, std::system_category(), "startup socket bind");
        return std::move(*listener);
    }

    ublk::StartupSnapshot snapshot() const noexcept {
        if (finished_.load(std::memory_order_acquire)) return terminal_;
        return startup_.snapshot();
    }

    static elio::coro::task<bool> send(elio::net::uds_stream& stream,
                                      nlohmann::json reply) {
        const auto line = reply.dump() + "\n";
        auto outcome = co_await elio::with_timeout(std::chrono::seconds(1),
            [&](elio::coro::cancel_token token) -> elio::coro::task<bool> {
                const auto written = co_await stream.write_exactly(
                    line.data(), line.size(), std::move(token));
                co_return written.result == static_cast<ssize_t>(line.size());
            });
        co_return outcome && *outcome;
    }

    elio::coro::task<void> handle(elio::net::uds_stream& stream) {
        // One bounded JSON-line request per connection. Timeouts include the
        // entire line, so a trickle sender cannot extend its admission slot.
        std::string line;
        line.reserve(4096);
        auto read = co_await elio::with_timeout(std::chrono::seconds(1),
            [&](elio::coro::cancel_token token) -> elio::coro::task<bool> {
                char bytes[512];
                while (line.size() <= 4096) {
                    const auto r = co_await stream.read(bytes, sizeof(bytes), token);
                    if (r.result <= 0) co_return false;
                    line.append(bytes, static_cast<size_t>(r.result));
                    if (const auto end = line.find('\n'); end != std::string::npos) {
                        if (end > 4096) co_return false;
                        line.resize(end);
                        co_return true;
                    }
                }
                co_return false;
            });
        if (!read || !*read) co_return;
        const auto command = nlohmann::json::parse(line, nullptr, false);
        nlohmann::json reply = {{"ok", false}, {"version", supervisor::kStartupOwnerVersion}};
        uint64_t accepted_sequence = 0;
        if (!command.is_object() || !command.contains("cmd") ||
            !command["cmd"].is_string()) {
            reply["error"] = "invalid startup command";
        } else if (command["cmd"] == "startup_status") {
            reply["ok"] = true;
        } else if (command["cmd"] == "startup_release") {
            const auto generation = command.find("generation");
            const auto sequence = command.find("seq");
            auto result = ublk::ReleaseRequestResult::stale;
            if (generation != command.end() && generation->is_number_unsigned() &&
                sequence != command.end() && sequence->is_number_unsigned()) {
                result = startup_.request_release(generation->get<uint64_t>(),
                                                  sequence->get<uint64_t>());
            }
            switch (result) {
            case ublk::ReleaseRequestResult::accepted:
                accepted_sequence = sequence->get<uint64_t>();
                reply["ok"] = true;
                reply["release"] = "pending";
                break;
            case ublk::ReleaseRequestResult::stale: reply["release"] = "stale"; break;
            case ublk::ReleaseRequestResult::busy: reply["release"] = "busy"; break;
            case ublk::ReleaseRequestResult::not_startup: reply["release"] = "not_startup"; break;
            }
        } else {
            reply["error"] = "unsupported startup command";
        }
        reply["startup_owner"] = supervisor::startup_owner_json(startup_status(snapshot()));
        const bool sent = co_await send(stream, std::move(reply));
        if (!sent || accepted_sequence == 0) co_return;

        // Acceptance is not retirement. Keep this connection for one outcome
        // line; a disconnect never revokes an already accepted release. There
        // can be only one unacknowledged owner request, so this wait does not
        // consume the acceptor or create unbounded release work.
        for (;;) {
            const bool terminal = finished_.load(std::memory_order_acquire);
            const auto facts = snapshot();
            const bool superseded = facts.release_sequence > accepted_sequence;
            const bool refused = facts.release_sequence == accepted_sequence &&
                                 facts.release == ublk::StartupRelease::refused;
            if (refused || superseded || terminal) {
                const bool released = terminal && facts.release_sequence == accepted_sequence &&
                                      facts.release == ublk::StartupRelease::released;
                nlohmann::json outcome = {
                    {"ok", released}, {"seq", accepted_sequence},
                    {"release", refused ? "refused" : released ? "released" : "superseded"},
                    {"startup_owner", supervisor::startup_owner_json(startup_status(facts))}};
                (void)co_await send(stream, std::move(outcome));
                co_return;
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ublk::StartupControl& startup_;
    elio::net::uds_listener listener_;
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    ublk::StartupSnapshot terminal_;
#ifdef OBD_TEST_HOOKS
    friend struct StartupSocketTestAccess;
    void (*before_join_)(size_t, void*) = nullptr;
    void* join_context_ = nullptr;
#endif
};

} // namespace obd::cmd
