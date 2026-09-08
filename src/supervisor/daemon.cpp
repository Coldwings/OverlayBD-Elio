// Supervisor daemon. See daemon.hpp and docs/supervisor.md.
#include "supervisor/daemon.hpp"

#include "common/errors.hpp"
#include "format/lsmt_rw.hpp"
#include "image/config.hpp"
#include "supervisor/child.hpp"
#include "supervisor/protocol.hpp"

#include <elio/coro/with_timeout.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/net/uds.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/sync/event.hpp>
#include <elio/sync/mutex.hpp>
#include <elio/time/timer.hpp>

#include <libgen.h>
#include <cstdio>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

/// Reads a small control-plane text file through the async IO backend
/// (image configs are a few KiB). Throws obd::error on failure.
elio::coro::task<std::string> read_text_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw_errno(errno, "cannot open " + path);
    std::string text;
    char buf[8192];
    for (;;) {
        const auto r = co_await elio::io::async_read(fd, buf, sizeof(buf), -1);
        if (r.result < 0) {
            const int e = static_cast<int>(-r.result);
            ::close(fd);
            throw_errno(e, "cannot read " + path);
        }
        if (r.result == 0) break;
        text.append(buf, static_cast<size_t>(r.result));
    }
    ::close(fd);
    co_return text;
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

/// RAII unlock for elio::sync::mutex (unlock() is synchronous; only lock()
/// is a co_await). Lets coroutine exit paths (co_return, exception unwind)
/// release a held per-entry mutex without manual bookkeeping.
struct SyncMutexGuard {
    elio::sync::mutex& m;
    ~SyncMutexGuard() { m.unlock(); }
};

class Daemon {
    /// Per-device bookkeeping (ADR-0010): the spec is the respawn
    /// template; dev_id is learned from the ready status's bdev path.
    ///
    /// ADR-0014 commit bookkeeping: `upper_path`/`upper_type` are recorded
    /// from the image config at create time (provenance — later edits to
    /// the config file must not redirect commit). `committing` (guarded by
    /// `mu_`, like `destroying`) rejects concurrent commits. `op_mu`
    /// serializes a commit's stop-and-seal (and destroy's stop) against a
    /// recovery respawn in supervise_entry, so a seal never runs while a
    /// recovery child is booting or alive. Lock order: `mu_` is never held
    /// while acquiring `op_mu`; brief `mu_` sections inside `op_mu` are
    /// fine (no one acquires `op_mu` while holding `mu_`).
    struct DeviceEntry {
        ChildSpec spec;
        std::shared_ptr<Child> child;
        int dev_id = -1;
        int recoveries = 0;   // completed respawns
        bool destroying = false;  // intentional teardown: never respawn
        std::string upper_path;   // <upper.dir>/overlaybd.rw when lsmt
        std::string upper_type;   // "" = no writable upper recorded
        bool image_config_ok = false;  // config parsed at create time
        bool committing = false;      // a commit is in flight
        elio::sync::mutex op_mu;      // commit/destroy vs recovery respawn
    };

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


        elio::signal::signal_set term_set;
        term_set.add(SIGTERM).add(SIGINT);
        elio::signal::signal_fd term_fd(term_set);

        elio::go([this, &listener]() -> elio::coro::task<void> {
            co_await accept_loop(*listener);
        });
        // go_to pins the reaper to worker 0: its signal_fd caches the
        // creating worker's io_context, and the reaper also awaits sync
        // primitives (mu_) whose wakeups could otherwise migrate it onto
        // a worker where the cached io_context is invalid.
        elio::go_to(0, [this]() -> elio::coro::task<void> {
            // signal_fd caches the creating worker's io_context; construct
            // it inside the coroutine that awaits it.
            elio::signal::signal_set chld_set;
            chld_set.add(SIGCHLD);
            elio::signal::signal_fd chld_fd(chld_set);
            co_await reaper(chld_fd);
            reaper_done_.set();
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
        // Wake the parked accept: closing the listener does NOT cancel an
        // in-flight accept SQE, but an incoming connection completes it.
        // Best-effort; the loop exits on stopping_ either way.
        {
            const int wake = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_un sa {};
                sa.sun_family = AF_UNIX;
                std::snprintf(sa.sun_path, sizeof(sa.sun_path), "%s",
                              cfg_.socket_path.c_str());
                ::connect(wake, reinterpret_cast<sockaddr*>(&sa),
                          sizeof(sa));
                ::close(wake);
            }
        }
        // Wake the reaper: it is parked in a signal wait with no cancel
        // path; a synthetic SIGCHLD makes it re-check stopping_ and exit.
        // Harmless: the waitpid scan just finds nothing.
        ::kill(::getpid(), SIGCHLD);
        // Join both before returning: detached tasks must not outlive the
        // scheduler (teardown drains forever on parked tasks).
        co_await accept_done_.wait();
        co_await reaper_done_.wait();
        listener->close();

        // Terminate children, then wait briefly for them to exit.
        std::vector<std::shared_ptr<Child>> snapshot;
        {
            co_await mu_.lock();
            for (auto& [id, entry] : children_) {
                entry->destroying = true;
                snapshot.push_back(entry->child);
            }
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
        accept_done_.set();
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
            else if (c == "commit") reply = co_await cmd_commit(*cmd);
            else if (c == "list") reply = co_await cmd_list();
            else if (c == "hello") reply = reply_hello();
            else reply = co_await cmd_status(*cmd);
        }
        co_await stream.write(reply);
        // One command per connection; the stream closes on destruction.
    }

    /// ADR-0010: supervise one device entry for its whole lifetime —
    /// monitor the current child, and when it dies unexpectedly (not a
    /// requested destroy, not daemon shutdown), replace it with a recovery
    /// child attaching to the same ublk device, bounded by
    /// cfg_.max_recovery_attempts. dev_id is learned here (not in
    /// cmd_create) so the exit path can never observe it unset.
    elio::coro::task<void> supervise_entry(std::shared_ptr<DeviceEntry> entry,
                                           int fd) {
        for (;;) {
            {
                LineReader reader(fd);
                for (;;) {
                    auto line = co_await reader.next();
                    if (!line) break;
                    auto st = parse_device_status(*line);
                    if (!st) {
                        ELIO_LOG_WARNING("device {}: malformed status line",
                                         entry->spec.id);
                        continue;
                    }
                    Child::Status cur = entry->child->status();
                    cur.state = st->state;
                    if (!st->device.empty()) cur.device = st->device;
                    if (!st->error.empty()) cur.error = st->error;
                    entry->child->update_status(cur);
                    if (st->state == "ready" && entry->dev_id < 0 &&
                        !st->device.empty()) {
                        entry->dev_id = dev_id_from_bdev_path(st->device);
                        if (entry->dev_id < 0) {
                            ELIO_LOG_WARNING(
                                "device {}: cannot parse dev id from '{}'; "
                                "crash recovery disabled for this device",
                                entry->spec.id, st->device);
                        }
                    }
                    ELIO_LOG_INFO("device {} state: {} {}", entry->spec.id,
                                  st->state, st->device);
                }
                // EOF: the process is gone (or closing); mark exited if not
                // reaped yet. The reaper fills in the exit code on SIGCHLD.
                Child::Status cur = entry->child->status();
                if (cur.state != "exited") {
                    cur.state = "exited";
                    entry->child->update_status(cur);
                }
                ::close(fd);
            }
            if (stopping_.load()) co_return;
            {
                co_await mu_.lock();
                const bool destroy = entry->destroying;
                mu_.unlock();
                if (destroy) co_return;
            }
            if (entry->dev_id < 0 ||
                entry->recoveries >= cfg_.max_recovery_attempts) {
                ELIO_LOG_ERROR(
                    "device {} exited unexpectedly; no recovery left "
                    "(dev_id {}, recoveries {})",
                    entry->spec.id, entry->dev_id, entry->recoveries);
                co_return;
            }
            // Respawn in recovery mode: the kernel kept the ublk device
            // (created with UBLK_F_USER_RECOVERY), so the replacement
            // attaches instead of creating. op_mu serializes the respawn
            // against a concurrent commit/destroy (ADR-0014): either the
            // respawn completes first and the commit stops this child, or
            // the commit holds op_mu and the re-check below aborts the
            // respawn — a seal never runs while a recovery child is
            // booting or alive.
            co_await entry->op_mu.lock();
            {
                SyncMutexGuard op_guard{entry->op_mu};
                co_await mu_.lock();
                const bool destroy = entry->destroying;
                mu_.unlock();
                if (destroy) co_return;  // op_guard releases
                ChildSpec spec = entry->spec;
                spec.recover = true;
                spec.dev_id_request = entry->dev_id;
                std::shared_ptr<Child> next;
                try {
                    next = std::shared_ptr<Child>(
                        Child::spawn(spec).release());
                } catch (const std::system_error& e) {
                    ELIO_LOG_ERROR("device {} recovery spawn failed: {}",
                                   entry->spec.id, e.what());
                    co_return;
                }
                entry->recoveries += 1;
                ELIO_LOG_WARNING(
                    "device {} recovering via ublk USER_RECOVERY "
                    "(attempt {}, dev_id {})",
                    entry->spec.id, entry->recoveries, entry->dev_id);
                entry->child = next;
                fd = next->release_control_fd();
            }
        }
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
                for (auto& [id, entry] : children_) {
                    if (entry->child->pid() == pid) {
                        entry->child->note_reaped(wstatus);
                        ELIO_LOG_INFO("device {} exited (code {})", id,
                                      entry->child->status().exit_code);
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

        auto entry = std::make_shared<DeviceEntry>();
        entry->spec = ChildSpec{id, bin, config, global, dev_id, false};
        // ADR-0014: record the upper's path/kind for a later commit.
        // Provenance is the config as of create time — a later edit of the
        // config file must not redirect commit. A parse failure leaves the
        // entry without upper info (commit then reports it); the child
        // reports the config error itself.
        try {
            const std::string text = co_await read_text_file(config);
            const image::ImageConfig img = image::ImageConfig::from_json_text(
                text, source::DownloadConfig{});
            entry->image_config_ok = true;
            if (img.writable()) {
                entry->upper_type = img.upper.type;
                if (img.upper.type == "lsmt") {
                    entry->upper_path = img.upper.dir + "/overlaybd.rw";
                }
            }
        } catch (const std::exception& e) {
            ELIO_LOG_WARNING("device {}: cannot pre-parse image config ({})",
                             id, e.what());
        }
        try {
            entry->child =
                std::shared_ptr<Child>(Child::spawn(entry->spec).release());
        } catch (const std::system_error& e) {
            co_return reply_error(std::string("spawn failed: ") + e.what());
        }
        std::shared_ptr<Child> child = entry->child;
        {
            co_await mu_.lock();
            children_[id] = entry;
            mu_.unlock();
        }
        elio::go([this, entry, fd = entry->child->release_control_fd()]()
                 -> elio::coro::task<void> {
            co_await supervise_entry(entry, fd);
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
        std::shared_ptr<DeviceEntry> entry;
        {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) {
                entry = it->second;
                // Intentional: supervise_entry must not respawn. Written
                // under mu_ — supervise_entry reads it under mu_ too.
                entry->destroying = true;
            }
            mu_.unlock();
        }
        if (!entry) co_return reply_error("no such device: " + id);

        // op_mu serializes the stop against a recovery respawn in flight.
        co_await entry->op_mu.lock();
        {
            SyncMutexGuard op_guard{entry->op_mu};
            std::shared_ptr<Child> child = entry->child;
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
        }
        {
            co_await mu_.lock();
            children_.erase(id);
            mu_.unlock();
        }
        co_return reply_ok({{"id", id}});
    }

    /// ADR-0014 offline commit: stop the device (when live), then seal its
    /// LSMT-RW upper in this process and reply with the sealed file's path,
    /// sha256 and size. The upper path/kind recorded at create time is the
    /// provenance (docs/supervisor.md). Sparse uppers and upper-less
    /// devices are clear errors (upstream #216 parity). Concurrent commits
    /// of the same device are rejected (`committing`); the stop-and-seal
    /// itself is serialized against recovery respawns by the entry's
    /// op_mu, so a seal never runs while a device child is alive.
    elio::coro::task<std::string> cmd_commit(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        const std::string user_tag = j.value("user_tag", "");
        std::shared_ptr<DeviceEntry> entry;
        {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) {
                entry = it->second;
                if (entry->committing) {
                    mu_.unlock();
                    co_return reply_error("commit already in progress: " +
                                          id);
                }
                entry->committing = true;
                // Intentional stop: supervise_entry must not respawn.
                entry->destroying = true;
            }
            mu_.unlock();
        }
        if (!entry) co_return reply_error("no such device: " + id);

        std::string reply;
        try {
            reply = co_await commit_stop_and_seal(entry, id, user_tag);
        } catch (const std::exception& e) {
            reply = reply_error(std::string("commit failed for ") + id +
                                ": " + e.what());
        }
        {
            co_await mu_.lock();
            entry->committing = false;
            mu_.unlock();
        }
        co_return reply;
    }

    /// The commit critical section (see cmd_commit). Runs with the entry's
    /// op_mu held across the stop and the seal.
    elio::coro::task<std::string> commit_stop_and_seal(
        const std::shared_ptr<DeviceEntry>& entry, const std::string& id,
        const std::string& user_tag) {
        if (!entry->image_config_ok) {
            co_return reply_error(
                "image config unreadable at create; upper unknown: " + id);
        }
        if (entry->upper_type.empty()) {
            co_return reply_error("device has no writable upper: " + id);
        }
        if (entry->upper_type != "lsmt") {
            co_return reply_error("sparse uppers cannot be sealed: " + id);
        }
        const std::string& upper = entry->upper_path;

        co_await entry->op_mu.lock();
        SyncMutexGuard op_guard{entry->op_mu};

        // Stopped-device contract (ADR-0014): commit stops a live device
        // first (SIGTERM + bounded reap — the destroy mechanics without
        // removing the entry) and only then seals; a device that will not
        // stop is never sealed underneath. The graceful shutdown makes the
        // device checkpoint its upper's index (obd-device), which the
        // offline seal below requires. The loop re-reads entry->child: a
        // recovery respawn that completed just before op_mu was taken
        // installed a fresh child, which is stopped here too; while op_mu
        // is held no further respawn can install another.
        for (int attempt = 0;; ++attempt) {
            std::shared_ptr<Child> child = entry->child;
            if (child->status().state == "exited") break;
            if (attempt >= 3) {
                co_return reply_error(
                    "device keeps being replaced; upper not sealed: " + id);
            }
            child->terminate();
            auto done = co_await elio::with_timeout(
                std::chrono::seconds(cfg_.stop_timeout_sec),
                [&child](elio::coro::cancel_token tok)
                    -> elio::coro::task<void> {
                    co_await child->exit_event().wait(std::move(tok));
                });
            if (!done) {
                child->kill();
                auto gone = co_await elio::with_timeout(
                    std::chrono::seconds(2),
                    [&child](elio::coro::cancel_token tok)
                        -> elio::coro::task<void> {
                        co_await child->exit_event().wait(std::move(tok));
                    });
                if (!gone) {
                    co_return reply_error(
                        "device did not stop in time; upper not sealed: " +
                        id);
                }
            }
        }

        std::string sha256;
        uint64_t size = 0;
        const int rc = co_await format::LsmtRwLayer::seal_file(
            upper, user_tag, &sha256, &size);
        if (rc == -ENOENT) {
            co_return reply_error("upper file not found: " + upper);
        }
        if (rc == -EALREADY) {
            co_return reply_error("upper already sealed: " + upper);
        }
        if (rc == -EINVAL) {
            co_return reply_error("upper has no valid shutdown checkpoint "
                                  "(device crashed or was killed before "
                                  "stopping): " + upper);
        }
        if (rc != 0) {
            co_return reply_error(std::string("seal failed for ") + upper +
                                  ": " + std::strerror(-rc));
        }
        nlohmann::json fields;
        fields["id"] = id;
        fields["path"] = upper;
        fields["sha256"] = sha256;
        fields["size"] = size;
        co_return reply_ok(fields);
    }

    elio::coro::task<std::string> cmd_list() {
        nlohmann::json arr = nlohmann::json::array();
        co_await mu_.lock();
        for (auto& [id, entry] : children_) {
            const Child::Status st = entry->child->status();
            arr.push_back({{"id", id},
                           {"pid", entry->child->pid()},
                           {"state", st.state},
                           {"device", st.device},
                           {"error", st.error},
                           {"recoveries", entry->recoveries}});
        }
        mu_.unlock();
        co_return reply_ok({{"devices", arr}});
    }

    elio::coro::task<std::string> cmd_status(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        std::shared_ptr<DeviceEntry> entry;
        {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) entry = it->second;
            mu_.unlock();
        }
        if (!entry) co_return reply_error("no such device: " + id);
        const Child::Status st = entry->child->status();
        nlohmann::json fields;
        fields["id"] = id;
        fields["pid"] = entry->child->pid();
        fields["state"] = st.state;
        fields["device"] = st.device;
        fields["error"] = st.error;
        fields["exit_code"] = st.exit_code;
        fields["recoveries"] = entry->recoveries;
        co_return reply_ok(fields);
    }

    DaemonConfig cfg_;
    elio::sync::event accept_done_;
    elio::sync::event reaper_done_;
    elio::sync::mutex mu_;
    std::map<std::string, std::shared_ptr<DeviceEntry>> children_;
    std::atomic<bool> stopping_{false};
};

}  // namespace

elio::coro::task<int> run_daemon(const DaemonConfig& cfg) {
    Daemon d(cfg);
    co_return co_await d.run();
}

}  // namespace obd::supervisor
