// Supervisor daemon. See daemon.hpp and docs/supervisor.md.
#include "supervisor/daemon.hpp"

#include "common/errors.hpp"
#include "supervisor/child.hpp"
#include "supervisor/protocol.hpp"

#include <elio/coro/with_timeout.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/net/uds.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/sync/mutex.hpp>
#include <elio/time/timer.hpp>

#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace obd::supervisor {

namespace {

std::string default_device_bin() {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "obd-device";
    buf[n] = '\0';
    std::string dir = ::dirname(buf);
    return dir + "/obd-device";
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

/// Buffered JSON-lines reader over a raw fd (Elio IO backend).
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    /// Next line without the trailing '\n'; std::nullopt on EOF or error.
    elio::coro::task<std::optional<std::string>> next() {
        for (;;) {
            if (const auto nl = buf_.find('\n'); nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                co_return line;
            }
            if (buf_.size() > kMaxMessageBytes) co_return std::nullopt;
            char tmp[4096];
            const auto r =
                co_await elio::io::async_read(fd_, tmp, sizeof(tmp), -1);
            if (r.result <= 0) co_return std::nullopt;
            buf_.append(tmp, static_cast<size_t>(r.result));
        }
    }

private:
    int fd_;
    std::string buf_;
};

class Daemon {
public:
    explicit Daemon(DaemonConfig cfg) : cfg_(std::move(cfg)) {
        if (cfg_.device_bin.empty()) {
            cfg_.device_bin = default_device_bin();
        }
    }

    elio::coro::task<int> run() {
        auto listener = elio::net::uds_listener::bind(
            elio::net::unix_address(cfg_.socket_path));
        if (!listener) {
            ELIO_LOG_ERROR("cannot bind {}: {}", cfg_.socket_path,
                           std::strerror(errno));
            co_return 1;
        }
        ELIO_LOG_INFO("supervisor listening on {} (device bin: {})",
                      cfg_.socket_path, cfg_.device_bin);

        elio::signal::signal_set chld_set;
        chld_set.add(SIGCHLD);
        elio::signal::signal_fd chld_fd(chld_set);
        elio::signal::signal_set term_set;
        term_set.add(SIGTERM).add(SIGINT);
        elio::signal::signal_fd term_fd(term_set);

        elio::go([this, &listener]() -> elio::coro::task<void> {
            co_await accept_loop(*listener);
        });
        elio::go([this, &chld_fd]() -> elio::coro::task<void> {
            co_await reaper(chld_fd);
        });

        // Wait for the shutdown signal.
        for (;;) {
            auto info = co_await term_fd.wait();
            if (info && (info->signo == SIGTERM || info->signo == SIGINT)) {
                break;
            }
        }
        ELIO_LOG_INFO("supervisor shutting down");
        stopping_.store(true);
        listener->close();

        // Terminate children, then wait briefly for them to exit.
        std::vector<std::shared_ptr<Child>> snapshot;
        {
            co_await mu_.lock();
            for (auto& [id, child] : children_) snapshot.push_back(child);
            mu_.unlock();
        }
        for (auto& child : snapshot) child->terminate();
        for (auto& child : snapshot) {
            co_await elio::with_timeout(
                std::chrono::seconds(cfg_.stop_timeout_sec),
                [&child](elio::coro::cancel_token tok)
                    -> elio::coro::task<void> {
                    co_await child->exit_event().wait(std::move(tok));
                });
        }
        co_return 0;  // remaining children are SIGKILLed by ~Child
    }

private:
    elio::coro::task<void> accept_loop(elio::net::uds_listener& listener) {
        while (!stopping_.load()) {
            auto stream = co_await listener.accept();
            if (!stream) {
                if (stopping_.load()) break;
                ELIO_LOG_WARNING("accept failed: {}", std::strerror(errno));
                continue;
            }
            elio::go([this, s = std::move(*stream)]() mutable
                     -> elio::coro::task<void> {
                co_await handle_client(std::move(s));
            });
        }
    }

    elio::coro::task<void> handle_client(elio::net::uds_stream stream) {
        LineReader reader(stream.fd());
        std::string reply;
        auto line = co_await reader.next();
        if (!line) co_return;
        std::string error;
        auto cmd = parse_command(*line, error);
        if (!cmd) {
            reply = reply_error(error);
        } else {
            const std::string c = (*cmd)["cmd"].get<std::string>();
            if (c == "create") reply = co_await cmd_create(*cmd);
            else if (c == "destroy") reply = co_await cmd_destroy(*cmd);
            else if (c == "list") reply = co_await cmd_list();
            else reply = co_await cmd_status(*cmd);
        }
        co_await stream.write(reply);
        // One command per connection; the stream closes on destruction.
    }

    elio::coro::task<void> monitor_child(std::shared_ptr<Child> child,
                                         int fd) {
        LineReader reader(fd);
        for (;;) {
            auto line = co_await reader.next();
            if (!line) break;
            auto st = parse_device_status(*line);
            if (!st) {
                ELIO_LOG_WARNING("device {}: malformed status line",
                                 child->id());
                continue;
            }
            Child::Status cur = child->status();
            cur.state = st->state;
            if (!st->device.empty()) cur.device = st->device;
            if (!st->error.empty()) cur.error = st->error;
            child->update_status(cur);
            ELIO_LOG_INFO("device {} state: {} {}", child->id(), st->state,
                          st->device);
        }
        // EOF: the process is gone (or closing); mark exited if not reaped
        // yet. The reaper fills in the exit code when it sees SIGCHLD.
        Child::Status cur = child->status();
        if (cur.state != "exited") {
            cur.state = "exited";
            child->update_status(cur);
        }
        ::close(fd);
    }

    elio::coro::task<void> reaper(elio::signal::signal_fd& sigfd) {
        while (!stopping_.load()) {
            auto info = co_await sigfd.wait();
            if (!info) break;
            for (;;) {
                int wstatus = 0;
                const pid_t pid = ::waitpid(-1, &wstatus, WNOHANG);
                if (pid <= 0) break;
                co_await mu_.lock();
                for (auto& [id, child] : children_) {
                    if (child->pid() == pid) {
                        child->note_reaped(wstatus);
                        ELIO_LOG_INFO("device {} exited (code {})", id,
                                      child->status().exit_code);
                        break;
                    }
                }
                mu_.unlock();
            }
        }
    }

    elio::coro::task<std::string> cmd_create(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        const std::string config = j["config"].get<std::string>();
        const std::string global = j.value("global", cfg_.global_config);
        const std::string bin = j.value("device_bin", cfg_.device_bin);
        const int dev_id = j.value("dev_id", -1);

        if (id.empty() || id.find('/') != std::string::npos) {
            co_return reply_error("invalid id");
        }
        if (!file_exists(config)) {
            co_return reply_error("config file not found: " + config);
        }
        if (!file_exists(bin)) {
            co_return reply_error("obd-device binary not found: " + bin);
        }
        {
            co_await mu_.lock();
            const bool dup = children_.contains(id);
            mu_.unlock();
            if (dup) co_return reply_error("id already exists: " + id);
        }

        ChildSpec spec{id, bin, config, global, dev_id};
        std::shared_ptr<Child> child;
        try {
            child = std::shared_ptr<Child>(Child::spawn(spec).release());
        } catch (const std::system_error& e) {
            co_return reply_error(std::string("spawn failed: ") + e.what());
        }
        {
            co_await mu_.lock();
            children_[id] = child;
            mu_.unlock();
        }
        elio::go([this, child, fd = child->release_control_fd()]()
                 -> elio::coro::task<void> {
            co_await monitor_child(child, fd);
        });

        // Wait for the child to report ready/failed.
        auto outcome = co_await elio::with_timeout(
            std::chrono::seconds(cfg_.ready_timeout_sec),
            [&child](elio::coro::cancel_token tok)
                -> elio::coro::task<void> {
                co_await child->ready_event().wait(std::move(tok));
            });
        const Child::Status st = child->status();
        if (!outcome) {
            child->terminate();
            co_return reply_error("device did not become ready in time");
        }
        if (st.state != "ready") {
            child->terminate();
            co_return reply_error(st.error.empty()
                                      ? "device failed to start"
                                      : st.error);
        }
        nlohmann::json fields;
        fields["id"] = id;
        fields["pid"] = child->pid();
        fields["device"] = st.device;
        co_return reply_ok(fields);
    }

    elio::coro::task<std::string> cmd_destroy(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        std::shared_ptr<Child> child;
        {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) child = it->second;
            mu_.unlock();
        }
        if (!child) co_return reply_error("no such device: " + id);

        child->terminate();
        auto done = co_await elio::with_timeout(
            std::chrono::seconds(cfg_.stop_timeout_sec),
            [&child](elio::coro::cancel_token tok)
                -> elio::coro::task<void> {
                co_await child->exit_event().wait(std::move(tok));
            });
        if (!done) {
            child->kill();
            co_await elio::with_timeout(
                std::chrono::seconds(2),
                [&child](elio::coro::cancel_token tok)
                    -> elio::coro::task<void> {
                    co_await child->exit_event().wait(std::move(tok));
                });
        }
        {
            co_await mu_.lock();
            children_.erase(id);
            mu_.unlock();
        }
        co_return reply_ok({{"id", id}});
    }

    elio::coro::task<std::string> cmd_list() {
        nlohmann::json arr = nlohmann::json::array();
        co_await mu_.lock();
        for (auto& [id, child] : children_) {
            const Child::Status st = child->status();
            arr.push_back({{"id", id},
                           {"pid", child->pid()},
                           {"state", st.state},
                           {"device", st.device},
                           {"error", st.error}});
        }
        mu_.unlock();
        co_return reply_ok({{"devices", arr}});
    }

    elio::coro::task<std::string> cmd_status(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        std::shared_ptr<Child> child;
        {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) child = it->second;
            mu_.unlock();
        }
        if (!child) co_return reply_error("no such device: " + id);
        const Child::Status st = child->status();
        nlohmann::json fields;
        fields["id"] = id;
        fields["pid"] = child->pid();
        fields["state"] = st.state;
        fields["device"] = st.device;
        fields["error"] = st.error;
        fields["exit_code"] = st.exit_code;
        co_return reply_ok(fields);
    }

    DaemonConfig cfg_;
    elio::sync::mutex mu_;
    std::map<std::string, std::shared_ptr<Child>> children_;
    std::atomic<bool> stopping_{false};
};

}  // namespace

elio::coro::task<int> run_daemon(const DaemonConfig& cfg) {
    Daemon d(cfg);
    co_return co_await d.run();
}

}  // namespace obd::supervisor
