// Supervisor daemon. See daemon.hpp and docs/supervisor.md.
#include "supervisor/daemon.hpp"
#include "supervisor/daemon_test_hooks.hpp"

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
#include <mutex>
#include <optional>
#include <vector>
#include <utility>

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

// Defensive field readers for DEVICE-provided protocol JSON: value()
// throws type_error on a present-but-wrong-typed key, and a corrupted
// device reply must never tear down a routing coroutine. These return
// the default when the key is absent OR not of the expected type.
std::string str_or(const nlohmann::json& j, const char* key,
                   std::string def = "") {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>()
                                              : std::move(def);
}
uint64_t uint_or(const nlohmann::json& j, const char* key,
                 uint64_t def = 0) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number_unsigned()) {
        return it->get<uint64_t>();
    }
    if (it != j.end() && it->is_number_integer() && it->get<int64_t>() >= 0) {
        return static_cast<uint64_t>(it->get<int64_t>());
    }
    return def;
}
bool bool_or(const nlohmann::json& j, const char* key, bool def = false) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
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
    explicit LineReader(elio::net::uds_stream& stream,
                        elio::coro::cancel_token token = {})
        : stream_(stream), token_(std::move(token)) {}

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
                co_await stream_.read(tmp, sizeof(tmp), token_);
            if (r.result <= 0) co_return std::nullopt;
            buf_.append(tmp, static_cast<size_t>(r.result));
        }
    }

private:
    elio::net::uds_stream& stream_;
    elio::coro::cancel_token token_;
    std::string buf_;
};

/// RAII unlock for elio::sync::mutex (unlock() is synchronous; only lock()
/// is a co_await). Lets coroutine exit paths (co_return, exception unwind)
/// release a held per-entry mutex without manual bookkeeping.
struct SyncMutexGuard {
    elio::sync::mutex& m;
    ~SyncMutexGuard() { m.unlock(); }
};

// Own the scheduler wrapper as well as its coroutine result. A join_handle's
// ready state precedes callable/frame destruction; only is_destroyed proves
// that its raw-this captures have departed. Admission and closing share a lock.
// The brief timer is used only when draining, never to block a worker.
class OwnedTasks {
public:
    template<class F>
    void spawn(F&& function, std::optional<size_t> worker = {}) {
        std::lock_guard lock(mu_);
        if (closed_) throw std::logic_error("daemon task admission is closed");
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            if (it->is_destroyed()) {
                report(*it);
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
        // Reserve before scheduling: allocation failure must not leave an
        // already-running coroutine without a retained ownership handle.
        tasks_.reserve(tasks_.size() + 1);
        auto* scheduler = elio::runtime::scheduler::current();
        if (worker) {
            tasks_.push_back(scheduler->go_joinable_to(
                *worker, std::forward<F>(function)));
        } else {
            tasks_.push_back(scheduler->go_joinable(std::forward<F>(function)));
        }
    }

    elio::coro::task<void> join() {
        std::vector<elio::coro::join_handle<void>> tasks;
        {
            std::lock_guard lock(mu_);
            closed_ = true;
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) {
            while (!task.is_destroyed()) {
                bool timer_failed = false;
                try {
                    co_await elio::time::sleep_for(std::chrono::milliseconds(1));
                } catch (...) {
                    timer_failed = true;
                }
                // Rejected timer setup must not abandon the ownership drain.
                if (timer_failed) co_await elio::time::yield();
            }
            report(task);
        }
    }

private:
    static void report(elio::coro::join_handle<void>& task) {
        try {
            task.await_resume();
        } catch (const std::exception& error) {
            ELIO_LOG_ERROR("daemon task failed: {}", error.what());
        } catch (...) {
            ELIO_LOG_ERROR("daemon task failed with an unknown exception");
        }
    }
    std::mutex mu_;
    bool closed_ = false;
    std::vector<elio::coro::join_handle<void>> tasks_;
};

/// Default MkfsRunner (ADR-0014 mode 3): fork `mkfs.<type> <device>` and
/// reap it without blocking an Elio worker (a WNOHANG poll between async
/// sleeps, bounded by mkfs_timeout_sec — the daemon process blocks the
/// signalfd signals process-wide, and waitpid works independently of the
/// blocked SIGCHLD). `mkfs.<type>` is resolved on PATH by name; the type
/// charset is validated (valid_mkfs_type) before it reaches argv.
class ForkExecMkfsRunner final : public MkfsRunner {
public:
    explicit ForkExecMkfsRunner(int timeout_sec) : timeout_sec_(timeout_sec) {}

    elio::coro::task<int> run(const std::string& fs_type,
                              const std::string& device,
                              std::string* error) override {
        if (!valid_mkfs_type(fs_type)) {
            if (error) *error = "invalid fs type '" + fs_type + "'";
            co_return -EINVAL;
        }
        const std::string prog = "mkfs." + fs_type;
        const pid_t pid = ::fork();
        if (pid < 0) {
            if (error) {
                *error = std::string("fork for ") + prog + " failed: " +
                         std::strerror(errno);
            }
            const int e = -errno;
            co_return e;
        }
        if (pid == 0) {
            // Child: async-signal-safe only. execvp semantics with argv[0]
            // = the program name (PATH lookup happens in the child).
            ::execlp(prog.c_str(), prog.c_str(), device.c_str(),
                     static_cast<char*>(nullptr));
            _exit(127);
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(timeout_sec_);
        for (;;) {
            int wstatus = 0;
            const pid_t r = ::waitpid(pid, &wstatus, WNOHANG);
            if (r == pid) {
                if (WIFEXITED(wstatus)) {
                    const int code = WEXITSTATUS(wstatus);
                    if (code == 0) co_return 0;
                    if (code == 127 && error) {
                        *error = prog + ": not found or not executable";
                    } else if (error) {
                        *error = prog + " exited with code " +
                                 std::to_string(code);
                    }
                    co_return code;
                }
                if (WIFSIGNALED(wstatus)) {
                    if (error) {
                        *error = prog + " killed by signal " +
                                 std::to_string(WTERMSIG(wstatus));
                    }
                    co_return 128 + WTERMSIG(wstatus);
                }
                if (error) *error = prog + ": unexpected reap state";
                co_return -ECHILD;
            }
            if (r < 0 && errno != EINTR) {
                if (error) {
                    *error = std::string("waitpid for ") + prog +
                             " failed: " + std::strerror(errno);
                }
                const int e = -errno;
                co_return e;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(pid, SIGKILL);
                // Bounded reap, then hand the wait off if it is still not
                // done. SIGKILL normally lands immediately, but a helper
                // wedged in uninterruptible IO would otherwise stall this
                // coroutine — and with it the whole create — forever; the
                // create path must never park on it. WNOHANG polling for at
                // most ~2 s covers the ordinary case.
                bool reaped = false;
                for (int i = 0; i < 200; ++i) {
                    int ws = 0;
                    const pid_t got = ::waitpid(pid, &ws, WNOHANG);
                    if (got == pid || (got < 0 && errno != EINTR)) {
                        reaped = true;
                        break;
                    }
                    co_await elio::time::sleep_for(
                        std::chrono::milliseconds(10));
                }
                if (!reaped) {
                    // Hand the pid to the daemon's reaper rather than
                    // parking a detached task on it: a detached coroutine
                    // (or a blocking waitpid) would keep shutdown from
                    // draining deterministically and could pin a
                    // blocking-pool thread forever, while the reaper
                    // already wakes on SIGCHLD and can reap this helper the
                    // moment it finally dies — so it never becomes a zombie
                    // that outlives the device it was formatting.
                    std::lock_guard<std::mutex> lk(orphan_mu_);
                    orphans_.push_back(pid);
                }
                if (error) {
                    *error = prog + " timed out after " +
                             std::to_string(timeout_sec_) + " s";
                }
                co_return -ETIMEDOUT;
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::vector<pid_t> take_orphan_pids() override {
        std::lock_guard<std::mutex> lk(orphan_mu_);
        std::vector<pid_t> out;
        out.swap(orphans_);
        return out;
    }

private:
    int timeout_sec_;
    std::mutex orphan_mu_;
    std::vector<pid_t> orphans_;  // abandoned helpers awaiting a WNOHANG reap
};

class Daemon {
    // Protected by mu_. The handler owns this state after routing releases
    // admission, so later commands cannot replace its result or event.
    struct DeviceCommand {
        uint64_t seq = 0;
        elio::sync::event waiter;
        bool terminal = false;
        nlohmann::json reply;
    };

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
        // Initial assignment precedes registry insertion. Replacement holds
        // op_mu AND mu_; observers retain child/count/trace under mu_.
        // The sole monitor writer and op_mu-protected stop paths may also
        // read child directly because they cannot overlap replacement.
        std::shared_ptr<Child> child;
        int dev_id = -1;
        int recoveries = 0;   // completed respawns
        bool destroying = false;  // intentional teardown: never respawn
        std::string upper_path;   // <upper.dir>/overlaybd.rw when lsmt
        std::string upper_type;   // "" = no writable upper recorded
        /// True once the entry's writable upper is KNOWN — from the image
        /// config parsed at create time, or (blank devices, ADR-0014) from
        /// the workspace layout. Named for what commit actually needs; the
        /// old "image_config_ok" name lied for config-less blank devices.
        bool upper_known = false;
        // ADR-0014 blank provenance: no image config (the upper path comes
        // from the workspace layout); `mkfs_requested` records that the
        // create asked for host mkfs (mode 3) — such uppers are never
        // sealed (commit refuses). It is set BEFORE the entry is published
        // to `children_`, i.e. on the CREATE's INTENT, not on mkfs having
        // finished: the device is already reachable by a concurrent commit
        // while mkfs runs, and a flag that only flips afterwards would let
        // that commit seal a supervisor-formatted upper (the ADR-0014
        // boundary must not depend on a race).
        bool blank = false;
        bool mkfs_requested = false;
        bool committing = false;      // a commit is in flight
        elio::sync::mutex op_mu;      // commit/destroy vs recovery respawn

        // ADR-0013 trace recording (protocol v3). control owns the
        // supervisor end of the device command channel, published by
        // supervise_entry under mu_ (null when unavailable). Writers retain
        // the same stream until their in-flight I/O has completed. One device
        // command is outstanding at a time. The active slot releases on a
        // matching reply; the handler keeps its operation through consumption.
        // `trace` is null until the first trace_start.
        std::shared_ptr<elio::net::uds_stream> control;
        std::shared_ptr<DeviceCommand> active_command;
        uint64_t cmd_seq = 0;  // fresh correlation token for each admission
        nlohmann::json trace;
    };

public:
    explicit Daemon(DaemonConfig cfg, std::shared_ptr<detail::DaemonTestGate> gate = {})
        : cfg_(std::move(cfg)), test_gate_(std::move(gate)) {
        if (cfg_.device_bin.empty()) {
            cfg_.device_bin = default_device_bin();
        }
        // ADR-0014 mode 3: host mkfs runs only on an explicit blank.mkfs
        // request; the default runner is the fork/exec one (bounded by
        // mkfs_timeout_sec). Tests install a mock so the suite never
        // executes host mkfs.
        if (!cfg_.mkfs_runner) {
            cfg_.mkfs_runner =
                make_default_mkfs_runner(cfg_.mkfs_timeout_sec);
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

        // Allocate the drain coroutine frames before any child is admitted.
        auto accept_join = accept_tasks_.join();
        auto client_join = client_tasks_.join();
        auto monitor_join = monitor_tasks_.join();
        auto reaper_join = reaper_tasks_.join();
        std::exception_ptr failure;
        try {
            accept_tasks_.spawn([this, &listener]() -> elio::coro::task<void> {
                co_await accept_loop(*listener);
            });
            if (test_gate_ && test_gate_->is_armed("startup_failure")) {
                co_await test_gate_->observe("startup_failure", {});
                throw std::runtime_error("injected daemon startup failure");
            }
            // go_to pins the reaper to worker 0: its signal_fd caches the
            // creating worker's io_context, and the reaper also awaits sync
            // primitives (mu_) whose wakeups could otherwise migrate it onto
            // a worker where the cached io_context is invalid.
            reaper_tasks_.spawn([this]() -> elio::coro::task<void> {
                // signal_fd caches the creating worker's io_context; construct
                // it inside the coroutine that awaits it.
                elio::signal::signal_set chld_set;
                chld_set.add(SIGCHLD);
                elio::signal::signal_fd chld_fd(chld_set);
                co_await reaper(chld_fd);
            }, 0);

            // Wait for the shutdown signal.
            for (;;) {
                auto info = co_await term_fd.wait();
                if (info && (info->signo == SIGTERM || info->signo == SIGINT)) {
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ELIO_LOG_INFO("supervisor shutting down");
        stopping_.store(true);
        if (test_gate_) test_gate_->mark("shutdown");
        auto cancel = [&](elio::coro::cancel_source& source) {
            try {
                source.cancel();
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
        };
        cancel(accept_cancel_);
        cancel(client_cancel_);
        co_await accept_join;
        listener->close();
        // No accept can now add another handler. Admitted commands may still
        // create monitors or await child replies/reaping, so drain them first.
        co_await client_join;

        // Handlers can no longer change registry membership. Mark every
        // entry, then take op_mu so a recovery already past its stop check
        // finishes publishing before we choose the child to terminate.
        {
            co_await mu_.lock();
            SyncMutexGuard registry_guard{mu_};
            for (auto& [id, entry] : children_) entry->destroying = true;
        }
        for (auto& [id, entry] : children_) {
            co_await entry->op_mu.lock();
            SyncMutexGuard op_guard{entry->op_mu};
            entry->child->terminate();
        }
        for (auto& [id, entry] : children_) {
            co_await entry->op_mu.lock();
            SyncMutexGuard op_guard{entry->op_mu};
            auto child = entry->child;
            bool exited = false;
            try {
                const auto outcome = co_await elio::with_timeout(
                    std::chrono::seconds(cfg_.stop_timeout_sec),
                    [&child](elio::coro::cancel_token tok)
                        -> elio::coro::task<void> {
                        co_await child->exit_event().wait(std::move(tok));
                    });
                exited = static_cast<bool>(outcome);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            if (!exited) child->kill();
        }
        // A child (or an inherited socket in its descendants) need not close
        // the channel to let the monitor depart. Cancellation completes the
        // read before its shared stream owner can close the descriptor.
        cancel(monitor_cancel_);
        co_await monitor_join;
        reaper_stopping_.store(true);
        ::kill(::getpid(), SIGCHLD);
        co_await reaper_join;
        if (failure) std::rethrow_exception(failure);
        co_return 0;
    }

private:
    static std::shared_ptr<elio::net::uds_stream> take_channel(Child& child) {
        elio::net::uds_stream stream(child.release_control_fd());
        return std::make_shared<elio::net::uds_stream>(std::move(stream));
    }

    elio::coro::task<void> accept_loop(elio::net::uds_listener& listener) {
        while (!stopping_.load()) {
            auto stream = co_await listener.accept(accept_cancel_.get_token());
            if (!stream) {
                if (stopping_.load()) break;
                ELIO_LOG_WARNING("accept failed: {}", std::strerror(errno));
                continue;
            }
            if (stopping_.load()) break;
            if (test_gate_) co_await test_gate_->observe("accepted", {});
            auto handle = [this, s = std::move(*stream)]() mutable
                          -> elio::coro::task<void> {
                co_await handle_client(std::move(s));
            };
            // Test placement forces command/monitor overlap on distinct
            // workers without changing the normal daemon's scheduling.
            client_tasks_.spawn(std::move(handle),
                test_gate_ ? std::optional<size_t>{2} : std::nullopt);
        }
    }

    elio::coro::task<void> handle_client(elio::net::uds_stream stream) {
        if (test_gate_) test_gate_->mark("client_read");
        LineReader reader(stream, client_cancel_.get_token());
        std::string reply;
        auto line = co_await reader.next();
        if (!line || stopping_.load()) co_return;
        if (test_gate_) co_await test_gate_->observe("command", {});
        std::string error;
        auto cmd = parse_command(*line, error);
        if (!cmd) {
            reply = reply_error(error);
        } else {
            const std::string c = (*cmd)["cmd"].get<std::string>();
            // Exception boundary: the parser validates field types, but a
            // handler must never kill the client coroutine without the
            // promised clean error reply.
            try {
                if (c == "create") reply = co_await cmd_create(*cmd);
                else if (c == "destroy") reply = co_await cmd_destroy(*cmd);
                else if (c == "commit") reply = co_await cmd_commit(*cmd);
                else if (c == "trace_start")
                    reply = co_await cmd_trace_start(*cmd);
                else if (c == "trace_stop")
                    reply = co_await cmd_trace_stop(*cmd);
                else if (c == "resize") reply = co_await cmd_resize(*cmd);
                else if (c == "list") reply = co_await cmd_list();
                else if (c == "hello") reply = reply_hello();
                else reply = co_await cmd_status(*cmd);
            } catch (const std::exception& e) {
                reply = reply_error(std::string("internal error handling '") +
                                    c + "': " + e.what());
            }
        }
        co_await stream.write(reply, client_cancel_.get_token());
        // One command per connection; the stream closes on destruction.
    }

    /// ADR-0010: supervise one device entry for its whole lifetime —
    /// monitor the current child, and when it dies unexpectedly (not a
    /// requested destroy, not daemon shutdown), replace it with a recovery
    /// child attaching to the same ublk device, bounded by
    /// cfg_.max_recovery_attempts. dev_id is learned here (not in
    /// cmd_create) so the exit path can never observe it unset.
    elio::coro::task<void> supervise_entry(std::shared_ptr<DeviceEntry> entry,
                                           std::shared_ptr<elio::net::uds_stream> channel) {
        for (;;) {
            {
                {
                    co_await mu_.lock();
                    entry->control = channel;
                    mu_.unlock();
                }
                LineReader reader(*channel, monitor_cancel_.get_token());
                for (;;) {
                    auto line = co_await reader.next();
                    if (!line) break;
                    // ADR-0013: command replies and trace events carry
                    // the "reply" discriminator; everything else is a
                    // lifecycle status line as before.
                    {
                        nlohmann::json j = nlohmann::json::parse(
                            *line, nullptr, false);
                        if (!j.is_discarded() && is_device_reply_line(j)) {
                            co_await route_device_reply(entry, std::move(j));
                            if (test_gate_) {
                                test_gate_->mark("device_reply_routed");
                                co_await test_gate_->observe("device_reply", {});
                            }
                            continue;
                        }
                    }
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
                if (test_gate_) co_await test_gate_->observe("monitor_eof", entry->child);
                {
                    co_await mu_.lock();
                    entry->control.reset();
                    if (entry->active_command) {
                        auto operation = entry->active_command;
                        operation->reply =
                            {{"ok", false},
                             {"error", "device control channel closed"}};
                        operation->terminal = true;
                        entry->active_command.reset();
                        operation->waiter.set();
                    }
                    // Crash/exit mid-record: queued records were
                    // memory-only and died with the device — never
                    // leave "recording" standing (it would survive a
                    // recovery respawn of a device that records
                    // nothing).
                    if (entry->trace.is_object() &&
                        entry->trace.value("state", "") == "recording") {
                        entry->trace["state"] = "lost";
                        entry->trace["reason"] = "device_exit";
                    }
                    mu_.unlock();
                }
                if (test_gate_) test_gate_->mark("device_eof_completed");
                // EOF: the process is gone (or closing); mark exited if not
                // reaped yet. The reaper fills in the exit code on SIGCHLD.
                Child::Status cur = entry->child->status();
                if (cur.state != "exited") {
                    cur.state = "exited";
                    entry->child->update_status(cur);
                }
                channel.reset();
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
                if (destroy || stopping_.load()) co_return;  // op_guard releases
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
                // Keep the old owner alive until after mu_ is released:
                // Child destruction can kill/waitpid and must not block
                // the registry (nor may spawn or the fd handoff run there).
                std::shared_ptr<Child> previous;
                co_await mu_.lock();
                {
                    SyncMutexGuard registry_guard{mu_};
                    previous = std::exchange(entry->child, next);
                    entry->recoveries += 1;
                }
                previous.reset();
                ELIO_LOG_WARNING(
                    "device {} recovering via ublk USER_RECOVERY "
                    "(attempt {}, dev_id {})",
                    entry->spec.id, entry->recoveries, entry->dev_id);
                if (test_gate_) co_await test_gate_->observe("recovery", next);
                channel = take_channel(*next);
            }
        }
    }

    /// Reaps the daemon's own device children, one pid at a time.
    ///
    /// Deliberately NOT `waitpid(-1, ...)`: the daemon also forks
    /// out-of-band helper processes (the ADR-0014 mode-3 mkfs runner),
    /// whose exit status belongs to their owner. A wildcard wait would
    /// steal it, leaving the owner's `waitpid` with ECHILD — mode 3 would
    /// fail on every create. The reaper therefore knows exactly which
    /// pids are device children (the registry) and never touches anything
    /// else.
    elio::coro::task<void> reaper(elio::signal::signal_fd& sigfd) {
        while (!reaper_stopping_.load()) {
            auto info = co_await sigfd.wait();
            if (!info) break;
            // A coalesced SIGCHLD can cover several exits: sweep until a
            // full pass reaps nothing.
            for (;;) {
                std::vector<std::pair<std::string, std::shared_ptr<Child>>>
                    snapshot;
                {
                    co_await mu_.lock();
                    snapshot.reserve(children_.size());
                    for (auto& [id, entry] : children_) {
                        snapshot.emplace_back(id, entry->child);
                    }
                    mu_.unlock();
                }
                bool reaped_any = false;
                for (auto& [id, child] : snapshot) {
                    const pid_t pid = child->pid();
                    if (pid <= 0) continue;
                    int wstatus = 0;
                    const pid_t r = ::waitpid(pid, &wstatus, WNOHANG);
                    if (r != pid) continue;  // still alive, or already reaped
                    child->note_reaped(wstatus);
                    ELIO_LOG_INFO("device {} exited (code {})", id,
                                  child->status().exit_code);
                    reaped_any = true;
                }
                // Abandoned out-of-band helpers (a mode-3 mkfs that
                // outlived its bounded post-SIGKILL reap) are collected
                // here too: their owner has given up on them, and no other
                // wait may touch them (a wildcard wait would steal a LIVE
                // helper's status — see above), so the reaper is the only
                // place that can keep them from zombifying.
                for (const pid_t orphan :
                     cfg_.mkfs_runner->take_orphan_pids()) {
                    abandoned_helpers_.push_back(orphan);
                }
                for (auto it = abandoned_helpers_.begin();
                     it != abandoned_helpers_.end();) {
                    int wstatus = 0;
                    const pid_t r = ::waitpid(*it, &wstatus, WNOHANG);
                    // Erase on the pid (we reaped it) or on a TERMINAL error
                    // (ECHILD: it was reaped elsewhere; anything but EINTR
                    // means this wait will never succeed). Keeping such a pid
                    // would grow the list forever and re-issue a syscall for
                    // it on every SIGCHLD wake.
                    if (r == *it) {
                        ELIO_LOG_INFO(
                            "reaped abandoned mkfs helper (pid {})", *it);
                        it = abandoned_helpers_.erase(it);
                    } else if (r < 0 && errno != EINTR) {
                        ELIO_LOG_WARNING(
                            "abandoned mkfs helper pid {} is unreapable "
                            "(waitpid: {}); dropping it",
                            *it, std::strerror(errno));
                        it = abandoned_helpers_.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!reaped_any) break;
            }
        }
    }

    elio::coro::task<std::string> cmd_create(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        const bool blank_mode = j.contains("blank");
        std::optional<BlankSpec> blank;
        if (blank_mode) {
            std::string blank_err;
            blank = parse_blank_spec(j["blank"], blank_err);
            if (!blank) {
                co_return reply_error("invalid create: " + blank_err);
            }
        }
        const std::string config =
            blank_mode ? "" : j["config"].get<std::string>();
        const std::string global = j.value("global", cfg_.global_config);
        const std::string bin = j.value("device_bin", cfg_.device_bin);
        const int dev_id = j.value("dev_id", -1);
        // D3 create-time headroom (bytes; 0 = size the device to the
        // image). parse_command validated the type; positivity/alignment
        // are checked here, and grow-only vs the image's declared size is
        // enforced by the device after assembly (the supervisor cannot
        // know the image size without opening every layer).
        uint64_t virtual_size = 0;
        if (j.contains("virtual_size")) {
            virtual_size = j["virtual_size"].is_number_unsigned()
                               ? j["virtual_size"].get<uint64_t>()
                               : static_cast<uint64_t>(
                                     j["virtual_size"].get<int64_t>());
        }

        // The id becomes a per-device workspace path (<blank_dir>/<id>) for
        // blank creates, so "." / ".." are rejected alongside "/".
        if (id.empty() || id == "." || id == ".." ||
            id.find('/') != std::string::npos) {
            co_return reply_error("invalid id");
        }
        if (virtual_size > 0 && virtual_size % 512 != 0) {
            co_return reply_error(
                "create virtual_size must be a positive multiple of 512 "
                "bytes");
        }
        // A blank create carries no image config (the child builds the
        // blank stack); an image create must name an existing config.
        if (!blank_mode && !file_exists(config)) {
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
        if (blank_mode) {
            // ADR-0014 modes 2/3: no image config. The child builds the
            // blank stack (empty LSMT zero base + LSMT-RW upper) inside its
            // workspace; the upper path/kind below is the exact path the
            // device assembly uses, and commit provenance needs no config
            // pre-parse.
            ChildSpec spec;
            spec.id = id;
            spec.device_bin = bin;
            spec.global_path = global;
            spec.dev_id_request = dev_id;
            spec.blank = true;
            spec.blank_size = blank->size;
            spec.blank_dir = cfg_.blank_dir + "/" + id;
            entry->spec = std::move(spec);
            entry->upper_known = true;  // no config; upper known by layout
            entry->upper_type = "lsmt";
            entry->upper_path = cfg_.blank_dir + "/" + id + "/overlaybd.rw";
            entry->blank = true;
            // Recorded NOW (before the entry is published and before mkfs
            // runs) so the unsealable-upper rule holds in every ordering.
            entry->mkfs_requested = !blank->mkfs.empty();
        } else {
            ChildSpec spec;
            spec.id = id;
            spec.device_bin = bin;
            spec.config_path = config;
            spec.global_path = global;
            spec.dev_id_request = dev_id;
            // D3 create-time headroom: the device grows itself to this
            // override (grow-only vs the image's declared size).
            spec.virtual_size = virtual_size;
            entry->spec = std::move(spec);
            // ADR-0014: record the upper's path/kind for a later commit.
            // Provenance is the config as of create time — a later edit of
            // the config file must not redirect commit. A parse failure
            // leaves the entry without upper info (commit then reports it);
            // the child reports the config error itself.
            try {
                const std::string text = co_await read_text_file(config);
                const image::ImageConfig img =
                    image::ImageConfig::from_json_text(
                        text, image::DownloadConfig{});
                entry->upper_known = true;
                if (img.writable()) {
                    entry->upper_type = img.upper.type;
                    if (img.upper.type == "lsmt") {
                        entry->upper_path = img.upper.dir + "/overlaybd.rw";
                    }
                }
            } catch (const std::exception& e) {
                ELIO_LOG_WARNING(
                    "device {}: cannot pre-parse image config ({})", id,
                    e.what());
            }
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
        auto monitor = [this, entry, channel = take_channel(*entry->child)]() mutable
                       -> elio::coro::task<void> {
            if (test_gate_) test_gate_->mark("monitor_started");
            co_await supervise_entry(entry, std::move(channel));
            if (test_gate_) test_gate_->mark("monitor_departed");
        };
        monitor_tasks_.spawn(std::move(monitor),
            test_gate_ ? std::optional<size_t>{1} : std::nullopt);

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
        if (blank_mode) {
            fields["mode"] = "blank";
            fields["size"] = blank->size;
            // ADR-0014 mode 3: the host `mkfs.<type>` convenience runs only
            // when the create explicitly asked for it (never otherwise, and
            // never from tests — tests inject a mock runner). A failure
            // leaves a freshly created but unusable (unformatted) device:
            // stop it and remove the entry before replying with the error.
            if (!blank->mkfs.empty()) {
                if (st.device.empty() ||
                    dev_id_from_bdev_path(st.device) < 0) {
                    co_await stop_entry_and_erase(entry);
                    co_return reply_error(
                        "blank device reported no /dev/ublkbN path; "
                        "cannot run mkfs." +
                        blank->mkfs);
                }
                std::string mkfs_err;
                const int mkrc = co_await cfg_.mkfs_runner->run(
                    blank->mkfs, st.device, &mkfs_err);
                if (mkrc != 0) {
                    co_await stop_entry_and_erase(entry);
                    co_return reply_error(
                        "mkfs." + blank->mkfs + " on " + st.device +
                        " failed: " +
                        (mkfs_err.empty() ? std::to_string(mkrc)
                                          : mkfs_err));
                }
                // `mkfs_requested` was already set when the entry was
                // created (see DeviceEntry): the upper of a mode-3 device
                // is unsealable from the moment the create is visible.
                fields["mkfs"] = blank->mkfs;
            }
        }
        co_return reply_ok(fields);
    }

    /// Stops and removes EXACTLY the given device entry (used on
    /// create-time failures such as a failed mode-3 mkfs). The entry is
    /// passed in — not looked up by id — and re-checked under mu_ before
    /// anything is stopped or erased: a stale failure path (a create that
    /// spent minutes in mkfs while the id was destroyed and re-created)
    /// must never terminate or remove a NEWER device that happens to own
    /// the same id. Mirrors cmd_destroy otherwise: `destroying` set under
    /// mu_ so supervise_entry never respawns, op_mu around the stop, the
    /// entry erased (again identity-checked) at the end.
    elio::coro::task<void> stop_entry_and_erase(
        const std::shared_ptr<DeviceEntry>& entry) {
        if (!entry) co_return;
        {
            co_await mu_.lock();
            auto it = children_.find(entry->spec.id);
            if (it == children_.end() || it->second != entry) {
                // The id now belongs to a different (or no) device: the
                // registration of this stale entry is gone, and whatever
                // removed it (destroy) already stopped its child. Just
                // mark it so a late supervise_entry pass never respawns.
                entry->destroying = true;
                mu_.unlock();
                co_return;
            }
            entry->destroying = true;
            mu_.unlock();
        }
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
            auto it = children_.find(entry->spec.id);
            if (it != children_.end() && it->second == entry) {
                children_.erase(it);
            }
            mu_.unlock();
        }
        co_return;
    }

    /// Routes a device "reply"-discriminated line (ADR-0013): command
    /// replies complete the pending cmd handler; trace events update the
    /// additive `trace` status field.
    elio::coro::task<void> route_device_reply(
        const std::shared_ptr<DeviceEntry>& entry, nlohmann::json j) {
        // All fields below come from the DEVICE: protocol input. value()
        // throws type_error on a present-but-wrong-typed key, and a
        // corrupted/wedged device must not tear down the routing
        // coroutine (which would strand every later control command).
        // Parse defensively everywhere below.
        const std::string kind = j["reply"].get<std::string>();
        if (kind == "trace_event") {
            // Unsolicited (duration expiry): the recording is over.
            nlohmann::json t;
            t["state"] = "stopped";
            const auto eit = j.find("event");
            t["reason"] = (eit != j.end() && eit->is_string())
                              ? eit->get<std::string>()
                              : "expired";
            const auto pit = j.find("path");
            t["path"] = (pit != j.end() && pit->is_string())
                            ? pit->get<std::string>()
                            : "";
            t["sha256"] = str_or(j, "sha256");
            t["size"] = uint_or(j, "size");
            t["records"] = uint_or(j, "records");
            t["dropped"] = uint_or(j, "dropped");
            const auto oit = j.find("ok");
            const bool ok = (oit != j.end() && oit->is_boolean())
                                ? oit->get<bool>()
                                : false;
            if (!ok) t["error"] = str_or(j, "error");
            co_await mu_.lock();
            // Only the recording the event belongs to: a stale expiry
            // landing after a NEW recording started must not flip the
            // fresh "recording" status back to "stopped". (trace is
            // null json until the first trace_start — value() on null
            // throws.)
            if (entry->trace.is_object() &&
                entry->trace.value("state", "") == "recording") {
                entry->trace = std::move(t);
            } else {
                ELIO_LOG_WARNING(
                    "device {}: stale trace event ignored (state {})",
                    entry->spec.id,
                    entry->trace.is_object()
                        ? entry->trace.value("state", "none")
                        : "none");
            }
            mu_.unlock();
            co_return;
        }
        // trace_start/trace_stop reply (or garbage): hand to the pending
        // command handler if there is one — but only when the reply's
        // echoed `seq` matches the pending command's. A LATE reply to a
        // timed-out command carries an old seq and is dropped, never
        // completing the wrong command.
        co_await mu_.lock();
        if (entry->active_command) {
            // Protocol input: parse `seq` defensively — a wrong-typed
            // value() would throw type_error and tear down the routing
            // coroutine. Missing or non-integer seq never matches.
            uint64_t echoed = 0;
            const auto sit = j.find("seq");
            if (sit != j.end() && sit->is_number_unsigned()) {
                echoed = sit->get<uint64_t>();
            }
            auto operation = entry->active_command;
            if (echoed == operation->seq) {
                operation->reply = std::move(j);
                operation->terminal = true;
                entry->active_command.reset();
                operation->waiter.set();
            } else {
                ELIO_LOG_WARNING(
                    "device {}: dropping stale command reply (seq {}, "
                    "pending seq {})",
                    entry->spec.id, echoed, operation->seq);
            }
        }
        mu_.unlock();
        co_return;
    }

    /// Shared forward-and-await for device-executed commands (ADR-0013
    /// trace path; D3 resize): sends `cmd` to the device over the control
    /// channel and waits (bounded) for its reply line. The trace duration
    /// bound is enforced DEVICE-side, so a client disconnect is harmless —
    /// this timeout only covers a wedged/dead device.
    elio::coro::task<std::string> forward_device_command(
        const std::string& id, nlohmann::json cmd) {
        std::shared_ptr<DeviceEntry> entry;
        auto operation = std::make_shared<DeviceCommand>();
        std::shared_ptr<elio::net::uds_stream> channel;
        bool command_busy = false;  // admission-time snapshot, guarded by mu_
        uint64_t seq = 0;  // captured under mu_: never read the member
                           // unlocked (only this path writes it, but
                           // keep the lock discipline exact)
        {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) entry = it->second;
            command_busy = entry && entry->active_command;
            if (entry && !command_busy && entry->control) {
                operation->seq = ++entry->cmd_seq;
                seq = operation->seq;
                entry->active_command = operation;
                channel = entry->control;
            }
            mu_.unlock();
        }
        if (!entry) co_return reply_error("no such device: " + id);
        if (!channel) {
            if (test_gate_) co_await test_gate_->observe("command_rejected", {});
            co_return reply_error(
                command_busy
                    ? "another device command is in flight: " + id
                    : "device control channel unavailable: " + id);
        }
        auto fail_pending = [&]() -> elio::coro::task<void> {
            co_await mu_.lock();
            if (entry->active_command == operation) {
                operation->terminal = true;
                entry->active_command.reset();
            }
            mu_.unlock();
        };
        auto execute = [&]() -> elio::coro::task<std::string> {
            if (test_gate_) co_await test_gate_->observe("command_write", {});
            cmd["seq"] = seq;  // the device echoes it back
            const std::string line = cmd.dump() + "\n";
            // Loop the write: a short write on SOCK_STREAM is legal and
            // retryable (buffer pressure), not a hard failure — same rule
            // as the device side's ControlChannelWriter.
            size_t sent = 0;
            while (sent < line.size()) {
                const auto w = co_await channel->write(
                    line.data() + sent, line.size() - sent, client_cancel_.get_token());
                if (w.result < 0) {
                    co_await fail_pending();
                    co_return reply_error("cannot reach the device control "
                                          "channel: " + id);
                }
                if (w.result == 0) {
                    co_await fail_pending();
                    co_return reply_error("cannot reach the device control "
                                          "channel (short write): " + id);
                }
                sent += static_cast<size_t>(w.result);
            }
            // Resolve optional test configuration before constructing the
            // awaitable; the normal path never dereferences a test gate.
            std::chrono::milliseconds command_timeout{30000};
            if (test_gate_) command_timeout = test_gate_->command_timeout;
            auto got = co_await elio::with_timeout(
                command_timeout,
                [&operation](elio::coro::cancel_token tok)
                    -> elio::coro::task<void> {
                    co_await operation->waiter.wait(std::move(tok));
                });
            if (test_gate_) {
                test_gate_->mark(got ? "command_wait_completed" : "command_wait_timeout");
                co_await test_gate_->observe("command_result", {});
            }
            nlohmann::json reply;
            {
                co_await mu_.lock();
                SyncMutexGuard registry_guard{mu_};
                // Preserve the timeout winner even if a reply races collection.
                // Only this operation's slot may be retired by its handler.
                if (!got && entry->active_command == operation) {
                    operation->terminal = true;
                    entry->active_command.reset();
                }
                if (got && operation->terminal) reply = std::move(operation->reply);
            }
            if (!got) {
                co_return reply_error("device control channel timeout: " + id);
            }
            if (!bool_or(reply, "ok", false)) {
                co_return reply_error(str_or(
                    reply, "error", "device rejected the command"));
            }
            // The device reply fields INCLUDING the "reply" discriminator;
            // cmd_trace_start/stop erase it before forwarding to the
            // client.
            co_return reply.dump();
        };
        std::string result;
        std::exception_ptr failure;
        try { result = co_await execute(); }
        catch (...) { failure = std::current_exception(); }
        // Includes serialization/allocation and I/O exceptions after admission.
        // A previous terminal result or a newer operation is never modified.
        co_await fail_pending();
        if (failure) std::rethrow_exception(failure);
        co_return result;
    }

    /// ADR-0013 record path: start a server-side-duration-bounded trace
    /// recording in the device process. The reply carries the device's
    /// fields plus id; the additive `trace` status field starts
    /// tracking the recording.
    elio::coro::task<std::string> cmd_trace_start(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        nlohmann::json cmd = {{"cmd", "trace_start"},
                              {"path", j["path"]},
                              {"duration_sec", j["duration_sec"]}};
        std::string r = co_await forward_device_command(id, std::move(cmd));
        auto rj = nlohmann::json::parse(r, nullptr, false);
        if (!rj.is_discarded() && bool_or(rj, "ok", false)) {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) {
                it->second->trace = {{"state", "recording"},
                                     {"path", str_or(rj, "path")},
                                     {"duration_sec", uint_or(rj, "duration_sec")}};
            }
            mu_.unlock();
            rj.erase("reply");
            rj.erase("seq");  // internal routing token, not client API
            rj["id"] = id;
            rj["ok"] = true;
            co_return rj.dump() + "\n";
        }
        co_return r;
    }

    elio::coro::task<std::string> cmd_trace_stop(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        // Named local: a brace-init json temporary as a coroutine
        // argument trips GCC 12's "array used as initializer" bug.
        nlohmann::json cmd = {{"cmd", "trace_stop"}};
        std::string r = co_await forward_device_command(id, std::move(cmd));
        auto rj = nlohmann::json::parse(r, nullptr, false);
        if (!rj.is_discarded() && bool_or(rj, "ok", false)) {
            co_await mu_.lock();
            auto it = children_.find(id);
            if (it != children_.end()) {
                // A stop after the duration already finalized the
                // recording is a no-op: the expiry event has already
                // recorded the true cause, and overwriting it would
                // misreport a late trace_stop as the reason.
                const bool already_expired =
                    it->second->trace.is_object() &&
                    it->second->trace.value("state", "") == "stopped" &&
                    it->second->trace.value("reason", "") == "expired";
                nlohmann::json t;
                t["state"] = "stopped";
                t["reason"] = already_expired ? "expired" : "stopped";
                t["path"] = str_or(rj, "path");
                t["sha256"] = str_or(rj, "sha256");
                t["size"] = uint_or(rj, "size");
                t["records"] = uint_or(rj, "records");
                t["dropped"] = uint_or(rj, "dropped");
                it->second->trace = std::move(t);
            }
            mu_.unlock();
            rj.erase("reply");
            rj.erase("seq");  // internal routing token, not client API
            rj["id"] = id;
            rj["ok"] = true;
            co_return rj.dump() + "\n";
        }
        co_return r;
    }

    /// D3 grow-only online resize (ADR-0014 dev_size model): forwards the
    /// byte-count `size` to the device, whose executor enforces the
    /// grow-only rule (the current size is known only there) and issues
    /// the ublk UPDATE_SIZE. The reply carries the new size in bytes.
    /// Semantics validated here before forwarding: `size` is a positive
    /// multiple of 512 (ublk sector granularity); anything else is a
    /// clean error without a round-trip. Shrink attempts reach the device
    /// and are rejected there with a clear message.
    ///
    /// Deliberately NOT serialized against cmd_commit: commit stops the
    /// device over signals + the entry's op_mu while resize rides the
    /// device command channel, but every ordering is safe because the
    /// DEVICE arbitrates — a grow completing before its shutdown is
    /// followed by a checkpoint at the grown size (consistent pair), and
    /// once the shutdown begins the device rejects resizes with
    /// "device is shutting down; resize ignored" (see make_resize_apply),
    /// so a header rewrite can never land after the checkpoint trailer.
    /// A resize racing a stop may instead see the channel close, also a
    /// clean error (docs/supervisor.md).
    elio::coro::task<std::string> cmd_resize(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        // parse_command validated 'size' as a non-negative integer.
        const uint64_t size = j["size"].is_number_unsigned()
                                  ? j["size"].get<uint64_t>()
                                  : static_cast<uint64_t>(
                                        j["size"].get<int64_t>());
        if (size == 0 || size % 512 != 0) {
            co_return reply_error(
                "resize size must be a positive multiple of 512 bytes");
        }
        // Named local: a brace-init json temporary as a coroutine
        // argument trips GCC 12's "array used as initializer" bug.
        nlohmann::json cmd = {{"cmd", "resize"}, {"size", size}};
        std::string r = co_await forward_device_command(id, std::move(cmd));
        auto rj = nlohmann::json::parse(r, nullptr, false);
        if (!rj.is_discarded() && bool_or(rj, "ok", false)) {
            rj.erase("reply");
            rj.erase("seq");  // internal routing token, not client API
            rj["id"] = id;
            rj["ok"] = true;
            co_return rj.dump() + "\n";
        }
        co_return r;
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
        // D3 commit re-baseline: optional virtual_size override (bytes)
        // written into the sealed header (0 = keep the layer's declared
        // size). parse_command validated the type; alignment/positivity
        // are checked here (before the device is stopped); grow-only vs
        // the layer's declared size and content extent is validated in
        // the seal path, where the checkpoint is readable.
        uint64_t virtual_size = 0;
        if (j.contains("virtual_size")) {
            virtual_size = j["virtual_size"].is_number_unsigned()
                               ? j["virtual_size"].get<uint64_t>()
                               : static_cast<uint64_t>(
                                     j["virtual_size"].get<int64_t>());
        }
        if (virtual_size > 0 && virtual_size % 512 != 0) {
            co_return reply_error(
                "commit virtual_size must be a positive multiple of 512 "
                "bytes");
        }
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
            reply = co_await commit_stop_and_seal(entry, id, user_tag,
                                                  virtual_size);
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
    /// op_mu held across the stop and the seal. `virtual_size` is the D3
    /// re-baseline override (0 = keep the layer's declared size).
    elio::coro::task<std::string> commit_stop_and_seal(
        const std::shared_ptr<DeviceEntry>& entry, const std::string& id,
        const std::string& user_tag, uint64_t virtual_size) {
        if (!entry->upper_known) {
            co_return reply_error(
                "image config unreadable at create; upper unknown: " + id);
        }
        if (entry->upper_type.empty()) {
            co_return reply_error("device has no writable upper: " + id);
        }
        if (entry->upper_type != "lsmt") {
            co_return reply_error("sparse uppers cannot be sealed: " + id);
        }
        // ADR-0014 mode 3 boundary: a blank device the supervisor itself
        // was asked to format with host mkfs is never sealed. Host mkfs
        // output is non-deterministic (UUIDs, hash seeds, timestamps) and
        // ADR-0014 excludes it from image building — this is where the
        // daemon can enforce that its own convenience run never becomes an
        // image layer. The check keys on the create-time INTENT
        // (`mkfs_requested`), not on mkfs having finished: the entry is
        // reachable while its mkfs step is still running, and a commit
        // landing in that window must be refused just the same. Mode-2
        // blanks (the caller formats) remain committable.
        if (entry->blank && entry->mkfs_requested) {
            co_return reply_error(
                "device was created with host mkfs (create mode 3); its "
                "non-deterministic upper cannot be sealed: " + id);
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
        std::string seal_reject;
        const int rc = co_await format::LsmtRwLayer::seal_file(
            upper, user_tag, &sha256, &size, virtual_size, &seal_reject);
        if (rc == -ENOENT) {
            co_return reply_error("upper file not found: " + upper);
        }
        if (rc == -EALREADY) {
            co_return reply_error("upper already sealed: " + upper);
        }
        if (rc == -EINVAL && !seal_reject.empty()) {
            // D3 re-baseline rejected (grow-only/alignment): precise
            // reason; the upper is untouched and remains committable.
            co_return reply_error(seal_reject);
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

    // Owning observation of one published generation. Child status can
    // evolve afterwards, but pid/status always come from this same Child;
    // recovery count and trace are copied together under the registry lock.
    struct EntrySnapshot {
        std::string id;
        std::shared_ptr<Child> child;
        int recoveries;
        nlohmann::json trace;
    };

    elio::coro::task<nlohmann::json> status_fields(const EntrySnapshot& snapshot,
                                 bool include_exit_code) {
        const Child::Status st = snapshot.child->status();
        if (test_gate_) {
            co_await test_gate_->observe(
                include_exit_code ? "status" : "list", snapshot.child);
        }
        nlohmann::json fields = {{"id", snapshot.id},
                                 {"pid", snapshot.child->pid()},
                                 {"state", st.state},
                                 {"device", st.device},
                                 {"error", st.error},
                                 {"recoveries", snapshot.recoveries}};
        if (include_exit_code) fields["exit_code"] = st.exit_code;
        if (!snapshot.trace.is_null()) fields["trace"] = snapshot.trace;
        co_return fields;
    }

    elio::coro::task<std::string> cmd_list() {
        std::vector<EntrySnapshot> snapshots;
        co_await mu_.lock();
        {
            SyncMutexGuard registry_guard{mu_};
            snapshots.reserve(children_.size());
            for (const auto& [id, entry] : children_) {
                snapshots.push_back(
                    {id, entry->child, entry->recoveries, entry->trace});
            }
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& snapshot : snapshots) {
            auto fields = co_await status_fields(snapshot, false);
            arr.push_back(std::move(fields));
        }
        co_return reply_ok({{"devices", arr}});
    }

    elio::coro::task<std::string> cmd_status(const nlohmann::json& j) {
        const std::string id = j["id"].get<std::string>();
        std::optional<EntrySnapshot> snapshot;
        co_await mu_.lock();
        {
            SyncMutexGuard registry_guard{mu_};
            auto it = children_.find(id);
            if (it != children_.end()) {
                const auto& entry = it->second;
                snapshot.emplace(EntrySnapshot{
                    id, entry->child, entry->recoveries, entry->trace});
            }
        }
        if (!snapshot) co_return reply_error("no such device: " + id);
        auto fields = co_await status_fields(*snapshot, true);
        co_return reply_ok(fields);
    }

    DaemonConfig cfg_;
    std::shared_ptr<detail::DaemonTestGate> test_gate_;
    OwnedTasks accept_tasks_;
    OwnedTasks client_tasks_;
    OwnedTasks monitor_tasks_;
    OwnedTasks reaper_tasks_;
    elio::coro::cancel_source accept_cancel_;
    elio::coro::cancel_source client_cancel_;
    elio::coro::cancel_source monitor_cancel_;
    std::atomic<bool> reaper_stopping_{false};
    elio::sync::mutex mu_;
    std::map<std::string, std::shared_ptr<DeviceEntry>> children_;
    std::atomic<bool> stopping_{false};
    /// Abandoned mkfs-helper pids the reaper still owes a reap (see
    /// reaper()): drained on every SIGCHLD wake, including the synthetic
    /// one that unparks the reaper at shutdown.
    std::vector<pid_t> abandoned_helpers_;
};

}  // namespace

MkfsRunnerPtr make_default_mkfs_runner(int timeout_sec) {
    return std::make_shared<ForkExecMkfsRunner>(timeout_sec);
}

elio::coro::task<int> run_daemon(const DaemonConfig& cfg) {
    Daemon d(cfg);
    co_return co_await d.run();
}

elio::coro::task<int> detail::run_daemon_with_test_gate(
    const DaemonConfig& cfg, std::shared_ptr<DaemonTestGate> gate) {
    Daemon d(cfg, std::move(gate));
    co_return co_await d.run();
}

}  // namespace obd::supervisor
