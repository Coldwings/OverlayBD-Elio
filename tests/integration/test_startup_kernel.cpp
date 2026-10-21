// The production supervisor and device binaries own all kernel resources.
// This controller never fabricates completions or deletes a retained device.
#include "format/writer.hpp"
#include "supervisor/protocol.hpp"
#include "ublk/ctrl.hpp"
#include "ublk/startup_control.hpp"
#include "ublk/startup_test_hooks.hpp"
#include "../child_reap_evidence.hpp"
#include "../support.hpp"
#include "../ublk_prerequisites.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

extern char** environ;

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Kind = obd::ublk::detail::StartupTestKind;
using Event = obd::ublk::detail::StartupTestEvent;
using Log = obd::ublk::detail::StartupFixtureLog;

void require_fixture(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void write_text(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
    file.close();
    if (!file) throw std::runtime_error("cannot write fixture file: " + path);
}
uint64_t process_start_ticks(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(input, line);
    const auto end = line.rfind(')');
    if (end == std::string::npos) throw std::runtime_error("process identity unavailable");
    std::istringstream fields(line.substr(end + 1));
    std::string ignored;
    for (unsigned field = 3; field < 22; ++field) fields >> ignored;
    uint64_t ticks = 0;
    if (!(fields >> ticks) || ticks == 0) throw std::runtime_error("process identity invalid");
    return ticks;
}

class PinnedProcess {
public:
    PinnedProcess(pid_t pid, uint64_t start_ticks) : pid_(pid) {
        require_fixture(process_start_ticks(pid) == start_ticks, "process identity changed before pin");
        proc_ = ::open(("/proc/" + std::to_string(pid)).c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (proc_ < 0) throw std::system_error(errno, std::generic_category(), "pin process directory");
        pidfd_ = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
        if (pidfd_ < 0) {
            const int error = errno;
            ::close(proc_);
            throw std::system_error(error, std::generic_category(), "pidfd_open");
        }
        try { require_fixture(process_start_ticks(pid) == start_ticks, "process identity changed while pinning"); }
        catch (...) { ::close(proc_); ::close(pidfd_); throw; }
    }
    ~PinnedProcess() { ::close(pidfd_); ::close(proc_); }
    PinnedProcess(const PinnedProcess&) = delete;
    PinnedProcess& operator=(const PinnedProcess&) = delete;
    void signal(int number) const {
        if (::syscall(SYS_pidfd_send_signal, pidfd_, number, nullptr, 0) != 0)
            throw std::system_error(errno, std::generic_category(), "pidfd_send_signal");
    }
    bool reaped() const {
        // pidfd poll alone proves exit, not reap. Resolve through the pinned
        // original proc directory so a recycled numeric PID cannot satisfy it.
        const int stat = ::openat(proc_, "stat", O_RDONLY | O_CLOEXEC);
        if (stat >= 0) { ::close(stat); return false; }
        if (errno == ENOENT || errno == ESRCH) return true;
        throw std::system_error(errno, std::generic_category(), "observe pinned process");
    }
    pid_t pid() const noexcept { return pid_; }
private:
    pid_t pid_;
    int proc_ = -1;
    int pidfd_ = -1;
};

class PinnedNode {
public:
    explicit PinnedNode(std::string path) : path_(std::move(path)) {
        fd_ = ::open(path_.c_str(), O_PATH | O_CLOEXEC);
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "pin node");
        if (::fstat(fd_, &identity_) != 0) {
            const int error = errno;
            ::close(fd_);
            throw std::system_error(error, std::generic_category(), "pin node identity");
        }
    }
    ~PinnedNode() { ::close(fd_); }
    PinnedNode(PinnedNode&& other) noexcept
        : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1)), identity_(other.identity_) {}
    PinnedNode(const PinnedNode&) = delete;
    bool removed() const {
        struct stat current{};
        if (::stat(path_.c_str(), &current) != 0) {
            if (errno == ENOENT || errno == ENOTDIR) return true;
            throw std::system_error(errno, std::generic_category(), "observe node identity");
        }
        return current.st_dev != identity_.st_dev || current.st_ino != identity_.st_ino;
    }
private:
    std::string path_;
    int fd_;
    struct stat identity_{};
};

class EventLog {
public:
    // Publication of Child precedes exec/main initialization. Retry only absent
    // or incomplete logs; an initialized wrong identity is a hard failure.
    static bool initialized(const std::string& directory, pid_t pid, uint64_t generation) {
        const auto path = directory + "/" + std::to_string(pid) + "-" +
                          std::to_string(generation) + ".events";
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            if (errno == ENOENT) return false;
            throw std::system_error(errno, std::generic_category(), "probe event log");
        }
        struct stat st{};
        const int rc = ::fstat(fd, &st);
        if (rc != 0) {
            const int error = errno;
            ::close(fd);
            throw std::system_error(error, std::generic_category(), "stat event log");
        }
        if (st.st_size == 0) { ::close(fd); return false; }
        if (st.st_size != static_cast<off_t>(sizeof(Log))) {
            ::close(fd);
            throw std::runtime_error("event log probe has wrong size");
        }
        void* mapping = ::mmap(nullptr, sizeof(Log), PROT_READ, MAP_SHARED, fd, 0);
        const int error = errno;
        ::close(fd);
        if (mapping == MAP_FAILED)
            throw std::system_error(error, std::generic_category(), "probe event mapping");
        const auto* log = static_cast<const Log*>(mapping);
        const bool ready = log->initialized.load(std::memory_order_acquire) != 0;
        const bool identity = !ready || (log->magic == obd::ublk::detail::kStartupFixtureMagic &&
            log->pid == static_cast<uint64_t>(pid) && log->generation == generation);
        ::munmap(mapping, sizeof(Log));
        require_fixture(identity, "initialized replacement log has wrong identity");
        return ready;
    }
    EventLog(const std::string& directory, pid_t pid, uint64_t generation)
        : path_(directory + "/" + std::to_string(pid) + "-" + std::to_string(generation) + ".events") {
        const int fd = ::open(path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "open event log");
        struct stat st{};
        if (::fstat(fd, &st) != 0 || st.st_size != static_cast<off_t>(sizeof(Log))) {
            ::close(fd);
            throw std::runtime_error("event log has wrong size");
        }
        void* mapping = ::mmap(nullptr, sizeof(Log), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        const int error = errno;
        ::close(fd);
        if (mapping == MAP_FAILED) throw std::system_error(error, std::generic_category(), "map event log");
        log_ = static_cast<Log*>(mapping);
        if (!log_->initialized.load(std::memory_order_acquire) ||
            log_->magic != obd::ublk::detail::kStartupFixtureMagic ||
            log_->pid != static_cast<uint64_t>(pid) || log_->generation != generation) {
            ::munmap(log_, sizeof(Log));
            log_ = nullptr;
            throw std::runtime_error("event log identity/header invalid");
        }
    }
    ~EventLog() { if (log_) ::munmap(log_, sizeof(Log)); }
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;
    const Log& header() const { return *log_; }
    void open_queue_one() { log_->queue_one_open.store(1, std::memory_order_release); }
    std::vector<Event> events() const {
        require_fixture(!log_->overflow.load(std::memory_order_acquire), "event log overflow; evidence incomplete");
        const auto count = std::min<uint64_t>(log_->allocated.load(std::memory_order_acquire),
                                             log_->records.size());
        std::vector<Event> result;
        result.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            const auto& record = log_->records[static_cast<size_t>(i)];
            if (record.committed.load(std::memory_order_acquire) != i + 1) break;
            result.push_back(record.event);
        }
        return result;
    }
    bool has(Kind kind, uint32_t queue = UINT32_MAX, uint32_t opcode = UINT32_MAX) const {
        for (const auto& event : events())
            if (event.kind == kind && (queue == UINT32_MAX || event.queue == queue) &&
                (opcode == UINT32_MAX || event.opcode == opcode)) return true;
        return false;
    }
    bool failed_end() const {
        for (const auto& event : events())
            if (event.kind == Kind::control_finished &&
                event.opcode == UBLK_U_CMD_END_USER_RECOVERY && event.result < 0 &&
                (event.value == ETIMEDOUT || event.value == ECANCELED)) return true;
        return false;
    }
    bool control_succeeded(uint32_t opcode) const {
        uint64_t original = 0;
        for (const auto& event : events()) {
            if (event.opcode != opcode) continue;
            if (event.kind == Kind::control_original && event.result == 0)
                original = event.cookie;
            if (original && event.kind == Kind::control_finished &&
                event.cookie == original && event.result == 0)
                return true;
        }
        return false;
    }
    void dump() const {
        Json document;
        document["source_path"] = path_;
        document["magic"] = log_->magic;
        document["initialized"] = log_->initialized.load(std::memory_order_acquire);
        document["allocated"] = log_->allocated.load(std::memory_order_acquire);
        document["capacity"] = log_->records.size();
        document["generation"] = log_->generation;
        document["pid"] = log_->pid;
        document["process_start_ticks"] = log_->process_start_ticks;
        document["recovery"] = log_->recovery;
        document["overflow"] = log_->overflow.load();
        auto& records = document["events"] = Json::array();
        const auto count = std::min<uint64_t>(log_->allocated.load(), log_->records.size());
        for (uint64_t i = 0; i < count; ++i) {
            const auto& record = log_->records[static_cast<size_t>(i)];
            if (record.committed.load(std::memory_order_acquire) != i + 1) {
                document["uncommitted_sequences"].push_back(i + 1);
                continue;
            }
            const auto& event = record.event;
            records.push_back({{"sequence", i + 1}, {"monotonic_ns", record.monotonic_ns},
                {"kind", static_cast<uint32_t>(event.kind)}, {"queue", event.queue},
                {"opcode", event.opcode}, {"result", event.result}, {"flags", event.flags},
                {"cookie", event.cookie}, {"value", event.value},
                {"owner_flags", event.owner_flags}, {"release_sequence", event.release_sequence}});
        }
        document["committed_count"] = records.size();
        document["queue_one_open"] = log_->queue_one_open.load();
        write_text(path_ + ".json", document.dump(2));
    }
private:
    std::string path_;
    Log* log_ = nullptr;
};

Json rpc(const std::string& path, const Json& command) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "fixture RPC socket");
    try {
        const timeval timeout{command.value("cmd", "") == "create" ? 20 : 3, 0};
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
            throw std::system_error(errno, std::generic_category(), "fixture RPC timeout");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        require_fixture(path.size() < sizeof(address.sun_path), "fixture socket path too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::system_error(errno, std::generic_category(), "fixture RPC connect");
        const auto line = command.dump() + "\n";
        size_t sent = 0;
        while (sent < line.size()) {
            const auto n = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("fixture RPC send failed");
            sent += static_cast<size_t>(n);
        }
        std::string received;
        for (;;) {
            const auto newline = received.find('\n');
            if (newline != std::string::npos) {
                auto reply = Json::parse(received.substr(0, newline));
                ::close(fd);
                return reply;
            }
            require_fixture(received.size() < 65536, "fixture RPC record too large");
            char bytes[4096];
            const auto n = ::read(fd, bytes, sizeof(bytes));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("fixture RPC receive failed");
            received.append(bytes, static_cast<size_t>(n));
        }
    } catch (...) { ::close(fd); throw; }
}

template<class F>
void wait_for(F&& predicate, std::chrono::seconds timeout, const char* reason) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error(reason);
}

class KernelStartupFixture {
public:
    KernelStartupFixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "obd-startup-kernel-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* made = ::mkdtemp(writable.data());
        if (!made) throw std::system_error(errno, std::generic_category(), "fixture directory");
        directory_ = made;
        socket_ = directory_ + "/s.sock";
        write_text(directory_ + "/partial-recovery-v1", "two queues; depth four; recovery queue one gate\n");
        write_text(directory_ + "/global.json", "{\"ublkConfig\":{\"enableRecovery\":true}}\n");
        // A real local LSMT image goes through the production assembler.
        payload_ = obd::test::pattern_bytes(256 * 1024, 93);
        const auto raw = obd::test::write_file(directory_ + "/raw", payload_);
        const int input = ::open(raw.c_str(), O_RDONLY | O_CLOEXEC);
        if (input < 0) throw std::system_error(errno, std::generic_category(), "fixture image");
        try { obd::format::write_lsmt_single_layer(input, payload_.size(), directory_ + "/layer", {}); }
        catch (...) { ::close(input); throw; }
        ::close(input);
        write_text(directory_ + "/image.json", Json{{"lowers", Json::array({{{"file", directory_ + "/layer"}}})}}.dump());
    }
    ~KernelStartupFixture() {
        // No signal, STOP, DEL, gate opening, directory unlink or forced process
        // group cleanup here. An unresolved fixture remains an inspectable owner.
        if (!complete_) std::fprintf(stderr,
            "retained kernel fixture: supervisor=%ld io_reader=%ld directory=%s\n",
            static_cast<long>(supervisor_), static_cast<long>(reader_), directory_.c_str());
    }
    const std::string& directory() const { return directory_; }
    void run(bool maintenance) {
        start_supervisor();
        wait_for([&] {
            try { return call({{"cmd", "hello"}}).value("ok", false); }
            catch (...) { return false; }
        }, 5s, "supervisor did not listen");
        const auto created = call({{"cmd", "create"}, {"id", "partial"},
                                  {"config", directory_ + "/image.json"}});
        remember("create", created);
        require_fixture(created.value("ok", false), "original create failed");
        created_ = true;
        auto status = call({{"cmd", "status"}, {"id", "partial"}});
        remember("original", status);
        require_fixture(status.at("state") == "ready", "original not ready");
        const auto original_pid = status.at("pid").get<pid_t>();
        const auto original_generation = status.at("startup_owner").at("generation").get<uint64_t>();
        original_log_ = std::make_unique<EventLog>(directory_, original_pid, original_generation);
        original_ = std::make_unique<PinnedProcess>(original_pid, original_log_->header().process_start_ticks);
        id_ = status.at("startup_owner").at("device_id").get<uint32_t>();
        obd::ublk::Ctrl inspect;
        const auto actual = inspect.get_dev_info(id_);
        require_fixture(actual.nr_hw_queues == 2 && actual.queue_depth == 4, "kernel geometry is not two queues/depth four");
        constexpr uint64_t recovery_flags = UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE;
        require_fixture((actual.flags & recovery_flags) == recovery_flags, "kernel did not negotiate recovery/reissue");
        const auto suffix = std::to_string(id_);
        nodes_.emplace_back("/dev/ublkb" + suffix);
        nodes_.emplace_back("/dev/ublkc" + suffix);
        nodes_.emplace_back("/sys/class/ublk-char/ublkc" + suffix);
        read_original("/dev/ublkb" + suffix);
        original_->signal(SIGKILL); // The single intentional crash stimulus.
        crashed_ = true;
        wait_for([&] {
            status = call({{"cmd", "status"}, {"id", "partial"}});
            return status.value("ok", false) && status.value("pid", pid_t{-1}) > 0 &&
                   status.at("pid").get<pid_t>() != original_pid &&
                   status.contains("startup_owner") &&
                   status.at("startup_owner").at("generation").get<uint64_t>() != original_generation;
        }, 10s, "replacement was not published");
        replacement_pid_ = status.at("pid").get<pid_t>();
        generation_ = status.at("startup_owner").at("generation").get<uint64_t>();
        wait_for([&] {
            const auto current = call({{"cmd", "status"}, {"id", "partial"}});
            require_fixture(current.value("pid", pid_t{-1}) == replacement_pid_ &&
                current.contains("startup_owner") &&
                current.at("startup_owner").at("generation") == generation_,
                "replacement changed while waiting for initialized evidence");
            if (!EventLog::initialized(directory_, replacement_pid_, generation_)) return false;
            replacement_log_ = std::make_unique<EventLog>(directory_, replacement_pid_, generation_);
            replacement_ = std::make_unique<PinnedProcess>(replacement_pid_, replacement_log_->header().process_start_ticks);
            return true;
        }, 5s, "replacement event log was not initialized");
        require_fixture(replacement_log_->header().recovery == 1 &&
                        generation_ != original_generation, "replacement identity is not recovery");
        require_fixture(original_->reaped(), "old serving generation was replaced before actual reap");
        const int original_wait = wait_reap_evidence(*original_log_, "original");
        require_fixture(WIFSIGNALED(original_wait) && WTERMSIG(original_wait) == SIGKILL,
                        "original process was not collected with the intentional SIGKILL");
        // Reaping the old process does not finish the driver's delayed
        // quiesce work. Linux 6.6 checks dying servers every five seconds;
        // observe the separate production START_RECOVERY retry (30 seconds)
        // before applying the queue-publication observation deadline.
        wait_for([&] {
            const auto current = call({{"cmd", "status"}, {"id", "partial"}});
            require_fixture(current.value("pid", pid_t{-1}) == replacement_pid_ &&
                current.contains("startup_owner") &&
                current.at("startup_owner").at("generation") == generation_,
                "replacement changed while waiting for START_RECOVERY");
            return replacement_log_->control_succeeded(UBLK_U_CMD_START_USER_RECOVERY);
        }, 35s, "original START_RECOVERY did not succeed; fixture remains retained");
        wait_for([&] {
            return replacement_log_->has(Kind::initial_gate_enter, 1) &&
                   replacement_log_->has(Kind::initial_issued, 0);
        }, 5s, "partial publication boundary not reached");
        require_fixture(!replacement_log_->has(Kind::initial_issued, 1), "held queue issued prematurely");
        wait_for([&] { return replacement_log_->failed_end(); }, 15s,
                 "original END did not fail and settle; fixture remains retained");
        end_failed_ = true;
        require_fixture(replacement_log_->has(Kind::control_cancel,
                            UINT32_MAX, UBLK_U_CMD_END_USER_RECOVERY),
                        "END cancellation CQE was not observed");
        require_fixture(!replacement_log_->has(Kind::adopted), "failed END unexpectedly adopted device");
        wait_for([&] {
            status = call({{"cmd", "status"}, {"id", "partial"}});
            return status.value("state", "") == "failed" &&
                   status.at("startup_owner").at("phase") == "retained";
        }, 5s, "failed replacement was not reported retained");
        for (unsigned observation = 0; observation != 3; ++observation) {
            check_retained(status);
            const auto listed = call({{"cmd", "list"}});
            remember("retained-list", listed);
            require_fixture(listed.at("devices").size() == 1, "unexpected competing registry entry");
            const auto& item = listed.at("devices").at(0);
            require_fixture(item.at("pid") == replacement_pid_ && item.at("recoveries") == 1,
                            "list changed replacement generation");
            require_no_stop_or_delete();
            std::this_thread::sleep_for(30ms);
            status = call({{"cmd", "status"}, {"id", "partial"}});
        }
        if (maintenance) {
            supervisor_pin_->signal(SIGTERM);
            maintenance_ = true;
            wait_for([&] {
                status = call({{"cmd", "status"}, {"id", "partial"}});
                return status.value("shutdown_pending", false);
            }, 5s, "supervisor did not enter protected maintenance");
            check_retained(status);
            require_fixture(call({{"cmd", "hello"}}).value("ok", false), "maintenance listener lost");
        }
        const auto held_destroy = call({{"cmd", "destroy"}, {"id", "partial"}});
        remember("destroy-while-held", held_destroy);
        require_fixture(!held_destroy.value("ok", false), "destroy falsely succeeded before held submission");
        status = call({{"cmd", "status"}, {"id", "partial"}});
        remember("release-pending", status);
        require_fixture(status.at("pid") == replacement_pid_ && status.at("startup_protected") == true,
                        "pending release lost its owner");
        require_fixture(status.at("startup_owner").at("release_sequence") == 1 &&
                        status.at("startup_owner").at("release") == "pending",
                        "operator release was not admitted as the same pending attempt");
        require_no_stop_or_delete();
        require_fixture(!replacement_->reaped(), "retained replacement reaped before issuance");
        replacement_log_->open_queue_one();
        gate_opened_ = true;
        wait_for([&] { return replacement_log_->has(Kind::caller_terminal); }, 15s,
                 "released generation did not retire to caller terminal");
        validate_retirement();
        wait_for([&] { return replacement_->reaped(); }, 5s, "terminal replacement was not actually reaped");
        const int replacement_wait = wait_reap_evidence(*replacement_log_, "replacement");
        require_fixture(WIFEXITED(replacement_wait) && WEXITSTATUS(replacement_wait) == 0,
                        "retired replacement did not exit normally with status zero");
        if (!maintenance_) {
            const auto destroyed = call({{"cmd", "destroy"}, {"id", "partial"}});
            remember("destroy-after-retirement", destroyed);
            require_fixture(destroyed.value("ok", false), "reaped released entry was not removed");
            require_fixture(call({{"cmd", "list"}}).at("devices").empty(), "registry retained completed entry");
            supervisor_pin_->signal(SIGTERM);
        }
        wait_supervisor();
        require_fixture(nodes_removed(), "original kernel node identities remain");
        require_fixture(event_file_count() == 2, "unexpected extra device generation was spawned");
        complete_ = true;
    }
    bool finish_report(const std::string& failure) noexcept {
        try {
            bool exported = original_log_ && replacement_log_;
            report_["failure"] = failure;
            report_["complete"] = complete_;
            report_["maintenance"] = maintenance_;
            report_["original_pid"] = original_ ? original_->pid() : -1;
            report_["original_generation"] = original_log_ ? original_log_->header().generation : 0;
            report_["original_start_ticks"] = original_log_ ? original_log_->header().process_start_ticks : 0;
            report_["supervisor_pid"] = supervisor_;
            report_["reader_pid"] = reader_;
            report_["replacement_pid"] = replacement_pid_;
            report_["replacement_generation"] = generation_;
            report_["replacement_start_ticks"] = replacement_log_ ? replacement_log_->header().process_start_ticks : 0;
            report_["device_id"] = id_;
            report_["gate_opened"] = gate_opened_;
            report_["end_failed"] = end_failed_;
            report_["directory"] = directory_;
            if (original_log_) {
                try { original_log_->dump(); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["original_export_error"] = error.what();
                }
                try { export_reap_evidence(*original_log_, "original"); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["original_reap_export_error"] = error.what();
                }
            }
            if (replacement_log_) {
                try { replacement_log_->dump(); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["replacement_export_error"] = error.what();
                }
                try { export_reap_evidence(*replacement_log_, "replacement"); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["replacement_reap_export_error"] = error.what();
                }
            }
            report_["export_complete"] = exported;
            write_text(directory_ + "/result.json", report_.dump(2));
            return exported;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "fixture evidence export failed: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "fixture evidence export failed\n");
        }
        return false;
    }
    void attempt_supported_cleanup() noexcept {
        if (complete_ || reader_ > 0) return;
        // Never open the held queue while original END is unresolved: doing so
        // could turn the missing failure witness into a different success case.
        if (crashed_ && !end_failed_) return;
        try {
            if (created_) {
                (void)call({{"cmd", "destroy"}, {"id", "partial"}});
                if (end_failed_ && replacement_log_) {
                    replacement_log_->open_queue_one();
                    gate_opened_ = true;
                    wait_for([&] { return replacement_log_->has(Kind::caller_terminal); }, 15s,
                             "cleanup still owns kernel commands");
                    (void)call({{"cmd", "destroy"}, {"id", "partial"}});
                }
            }
            if (supervisor_pin_) supervisor_pin_->signal(SIGTERM);
            wait_supervisor();
        } catch (...) {} // Preserve the process, kernel resource and evidence.
    }
private:
    std::optional<obd::test::ChildReapSnapshot> read_reap_evidence(const EventLog& log) const {
        const auto& identity = log.header();
        auto evidence = obd::test::read_child_reap(directory_, identity.generation);
        if (evidence) {
            require_fixture(evidence->pid == identity.pid &&
                            evidence->generation == identity.generation &&
                            evidence->reaper_pid == static_cast<uint64_t>(supervisor_),
                            "collected process does not match the pinned owner and supervisor");
        }
        return evidence;
    }
    Json reap_document(const EventLog& log, const obd::test::ChildReapSnapshot& evidence) const {
        const int status = evidence.wait_status;
        return {{"source_path", directory_ + "/child-" +
                                 std::to_string(evidence.generation) + ".reap"},
                {"magic", evidence.magic}, {"version", evidence.version}, {"committed", 1},
                {"generation", evidence.generation}, {"reaper_pid", evidence.reaper_pid},
                {"pid", evidence.pid}, {"wait_status", status},
                // Start ticks come from the already pinned live process log;
                // the reaper records only the exact PID it actually collected.
                {"matched_process_start_ticks", log.header().process_start_ticks},
                {"exited", WIFEXITED(status)}, {"exit_code", WIFEXITED(status) ? WEXITSTATUS(status) : -1},
                {"signaled", WIFSIGNALED(status)}, {"term_signal", WIFSIGNALED(status) ? WTERMSIG(status) : 0}};
    }
    int wait_reap_evidence(const EventLog& log, const char* role) {
        std::optional<obd::test::ChildReapSnapshot> evidence;
        wait_for([&] {
            evidence = read_reap_evidence(log);
            return evidence.has_value();
        }, 5s, "actual child wait status was not committed by its supervisor");
        report_[std::string(role) + "_reap"] = reap_document(log, *evidence);
        return evidence->wait_status;
    }
    void export_reap_evidence(const EventLog& log, const char* role) {
        const auto evidence = read_reap_evidence(log);
        require_fixture(evidence.has_value(), "actual child wait status is missing");
        const auto document = reap_document(log, *evidence);
        report_[std::string(role) + "_reap"] = document;
        write_text(directory_ + "/child-" + std::to_string(evidence->generation) +
                   ".reap.json", document.dump(2));
    }
    Json call(const Json& command) const { return rpc(socket_, command); }
    void remember(const char* checkpoint, const Json& record) {
        report_["observations"].push_back({{"checkpoint", checkpoint}, {"record", record}});
    }
    void check_retained(const Json& status) {
        remember("retained-status", status);
        require_fixture(status.at("pid") == replacement_pid_ && status.at("recoveries") == 1 &&
                        status.at("startup_protected") == true, "retained identity/protection changed");
        const auto& owner = status.at("startup_owner");
        require_fixture(owner.at("generation") == generation_ && owner.at("phase") == "retained" &&
                        owner.at("control_pending") == false &&
                        owner.at("raw_published").get<uint64_t>() > owner.at("raw_retired").get<uint64_t>(),
                        "retained owner did not report unresolved original command debt");
        require_fixture(!replacement_->reaped(), "replacement disappeared while retained");
    }
    void require_no_stop_or_delete() const {
        require_fixture(!replacement_log_->has(Kind::control_prepared, UINT32_MAX, UBLK_U_CMD_STOP_DEV) &&
                        !replacement_log_->has(Kind::control_prepared, UINT32_MAX, UBLK_U_CMD_DEL_DEV),
                        "STOP/DEL attempted before explicit release and initial issuance");
    }
    bool nodes_removed() const {
        for (const auto& node : nodes_) if (!node.removed()) return false;
        return true;
    }
    size_t event_file_count() const {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory_))
            if (entry.path().extension() == ".events") ++count;
        return count;
    }
    void start_supervisor() {
        std::vector<std::string> args{OBD_TEST_SUPERVISOR_BIN, "--socket", socket_,
            "--global", directory_ + "/global.json", "--device-bin", OBD_TEST_DEVICE_BIN,
            "--ready-timeout", "15", "--stop-timeout", "1"};
        std::vector<char*> argv;
        for (auto& argument : args) argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::vector<std::string> environment;
        for (char** item = environ; *item; ++item)
            if (std::string_view(*item).find("OBD_TEST_STARTUP_FIXTURE_DIR=") != 0)
                environment.emplace_back(*item);
        environment.push_back("OBD_TEST_STARTUP_FIXTURE_DIR=" + directory_);
        std::vector<char*> envp;
        for (auto& item : environment) envp.push_back(item.data());
        envp.push_back(nullptr);
        posix_spawnattr_t attributes;
        posix_spawn_file_actions_t actions;
        require_fixture(::posix_spawnattr_init(&attributes) == 0, "spawn attributes failed");
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            ::posix_spawnattr_destroy(&attributes);
            throw std::runtime_error("spawn actions failed");
        }
        sigset_t empty, defaults;
        ::sigemptyset(&empty);
        ::sigemptyset(&defaults);
        ::sigaddset(&defaults, SIGTERM);
        ::sigaddset(&defaults, SIGINT);
        ::sigaddset(&defaults, SIGCHLD);
        int error = ::posix_spawnattr_setpgroup(&attributes, 0);
        if (!error) error = ::posix_spawnattr_setsigmask(&attributes, &empty);
        if (!error) error = ::posix_spawnattr_setsigdefault(&attributes, &defaults);
        if (!error) error = ::posix_spawnattr_setflags(&attributes,
            POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        if (!error) error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
            (directory_ + "/supervisor.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!error) error = ::posix_spawn(&supervisor_, OBD_TEST_SUPERVISOR_BIN,
                                          &actions, &attributes, argv.data(), envp.data());
        ::posix_spawn_file_actions_destroy(&actions);
        ::posix_spawnattr_destroy(&attributes);
        if (error) throw std::system_error(error, std::generic_category(), "spawn supervisor fixture");
        supervisor_pin_ = std::make_unique<PinnedProcess>(supervisor_, process_start_ticks(supervisor_));
    }
    void wait_supervisor() {
        if (supervisor_reaped_) return;
        require_fixture(supervisor_ > 0, "supervisor was not started");
        int status = 0;
        wait_for([&] {
            const auto result = ::waitpid(supervisor_, &status, WNOHANG);
            if (result == supervisor_) { supervisor_reaped_ = true; return true; }
            if (result < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category(), "reap supervisor");
            return false;
        }, 5s, "supervisor remains retained");
        require_fixture(WIFEXITED(status) && WEXITSTATUS(status) == 0, "supervisor exited unsuccessfully");
    }
    void read_original(const std::string& bdev) {
        void* memory = nullptr;
        if (::posix_memalign(&memory, 4096, 4096) != 0) throw std::bad_alloc();
        std::unique_ptr<void, decltype(&std::free)> buffer(memory, &std::free);
        int pipefd[2];
        if (::pipe2(pipefd, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "read fixture pipe");
        reader_ = ::fork();
        if (reader_ < 0) { ::close(pipefd[0]); ::close(pipefd[1]); throw std::runtime_error("read fixture fork failed"); }
        if (reader_ == 0) {
            ::close(pipefd[0]);
            const int fd = ::open(bdev.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
            const ssize_t got = fd >= 0 ? ::pread(fd, memory, 4096, 0) : -1;
            const ssize_t sent = got == 4096 ? ::write(pipefd[1], memory, 4096) : -1;
            if (fd >= 0) ::close(fd);
            ::close(pipefd[1]);
            _exit(sent == 4096 ? 0 : 1);
        }
        ::close(pipefd[1]);
        pollfd readable{pipefd[0], POLLIN, 0};
        const int polled = ::poll(&readable, 1, 5000);
        std::array<uint8_t, 4096> received{};
        const auto count = polled > 0 ? ::read(pipefd[0], received.data(), received.size()) : -1;
        ::close(pipefd[0]);
        if (count != static_cast<ssize_t>(received.size()))
            throw std::runtime_error("original real block read did not finish; reader/owner retained");
        int status = 0;
        wait_for([&] {
            const auto reaped = ::waitpid(reader_, &status, WNOHANG);
            if (reaped == reader_) return true;
            if (reaped < 0 && errno != EINTR)
                throw std::system_error(errno, std::generic_category(), "reap block reader");
            return false;
        }, 5s, "original block reader remains unreaped");
        reader_ = -1;
        require_fixture(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                        std::equal(received.begin(), received.end(), payload_.begin()),
                        "original block data does not match LSMT image");
    }
    void validate_retirement() {
        const auto events = replacement_log_->events();
        size_t stop = events.size(), remove = events.size(), terminal = events.size();
        unsigned stops = 0, deletions = 0;
        std::array<size_t, 2> issued{events.size(), events.size()};
        std::array<size_t, 2> bridges{events.size(), events.size()};
        std::array<size_t, 2> exited{events.size(), events.size()};
        std::array<size_t, 2> closed{events.size(), events.size()};
        using Key = std::pair<uint32_t, uint64_t>;
        std::map<Key, size_t> prepared;
        std::map<Key, std::pair<size_t, int>> completed;
        std::map<uint64_t, size_t> controls, originals, cancels, cancel_completions, finished;
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& event = events[i];
            auto unique_control = [&](auto& map) {
                require_fixture(map.emplace(event.cookie, i).second, "control cookie reused or completed twice");
            };
            if (event.kind == Kind::control_prepared) unique_control(controls);
            if (event.kind == Kind::control_original) unique_control(originals);
            if (event.kind == Kind::control_cancel_prepared) unique_control(cancels);
            if (event.kind == Kind::control_cancel) unique_control(cancel_completions);
            if (event.kind == Kind::control_finished) unique_control(finished);
            if (event.kind == Kind::control_prepared && event.opcode == UBLK_U_CMD_STOP_DEV) { stop = i; ++stops; }
            if (event.kind == Kind::control_prepared && event.opcode == UBLK_U_CMD_DEL_DEV) { remove = i; ++deletions; }
            if (event.kind == Kind::caller_terminal) {
                terminal = i;
                require_fixture(event.flags == static_cast<uint32_t>(obd::ublk::StartupPhase::released) &&
                                event.cookie == event.value && event.owner_flags == 1 &&
                                event.opcode == static_cast<uint32_t>(obd::ublk::StartupRelease::released) &&
                                event.release_sequence == 1,
                                "terminal caller has unresolved debt or mismatched release identity");
            }
            if (event.queue < 2) {
                if (event.kind == Kind::initial_issued) issued[event.queue] = i;
                if (event.kind == Kind::bridge_destroyed) bridges[event.queue] = std::min(bridges[event.queue], i);
                if (event.kind == Kind::owner_exit) {
                    require_fixture(event.value == 1, "queue thread returned without owner exit proof");
                    exited[event.queue] = i;
                }
                if (event.kind == Kind::cdev_closed) {
                    require_fixture(event.result == 0, "queue cdev close failed");
                    closed[event.queue] = i;
                }
                if (event.kind == Kind::raw_prepared) {
                    require_fixture(prepared.emplace(Key{event.queue, event.cookie}, i).second,
                                    "raw cookie reused");
                }
                if (event.kind == Kind::raw_cqe) {
                    require_fixture((event.flags & IORING_CQE_F_MORE) == 0, "nonterminal raw CQE");
                    require_fixture(completed.emplace(Key{event.queue, event.cookie}, std::make_pair(i, event.result)).second,
                                    "duplicate raw original CQE");
                }
            }
        }
        require_fixture(stops == 1 && deletions == 1 && stop < remove && remove < terminal,
                        "STOP/DEL/caller terminal ordering or count invalid");
        require_fixture(controls.size() == originals.size() && controls.size() == finished.size(),
                        "control commands lack original completion/settlement evidence");
        size_t recovery_started = events.size();
        unsigned recovery_successes = 0;
        for (const auto& [cookie, index] : originals) {
            (void)cookie;
            if (events[index].opcode == UBLK_U_CMD_START_USER_RECOVERY &&
                events[index].result == 0) {
                recovery_started = index;
                ++recovery_successes;
            }
        }
        require_fixture(recovery_successes == 1, "recovery requires exactly one successful START original");
        for (const auto& [cookie, index] : controls) {
            require_fixture(originals.contains(cookie) && finished.contains(cookie), "missing control original/settlement");
            const auto original_index = originals.at(cookie), finish_index = finished.at(cookie);
            const auto& original = events[original_index];
            const auto& finish = events[finish_index];
            require_fixture(index < original_index && original_index < finish_index &&
                original.opcode == events[index].opcode && finish.opcode == original.opcode &&
                original.result == finish.result && (original.flags & IORING_CQE_F_MORE) == 0,
                "control original and settlement disagree");
            if (original.opcode == UBLK_U_CMD_START_USER_RECOVERY)
                require_fixture(original.result == 0 ||
                    (original.result == -EBUSY && original_index < recovery_started),
                    "START_RECOVERY has an unexpected failure or retried after success");
            if (original.opcode == UBLK_U_CMD_STOP_DEV)
                require_fixture(original.result == 0, "STOP original failed");
            if (original.opcode == UBLK_U_CMD_DEL_DEV)
                require_fixture(original.result == 0 || original.result == -ENODEV, "DEL original failed");
        }
        require_fixture(cancels.size() == cancel_completions.size(), "cancellation lacks its own terminal CQE");
        for (const auto& [cookie, index] : cancels) {
            require_fixture(cancel_completions.contains(cookie), "missing cancellation CQE");
            const auto completion_index = cancel_completions.at(cookie);
            const auto& cancel = events[completion_index];
            require_fixture(index < completion_index && cancel.value == events[index].value &&
                cancel.opcode == events[index].opcode && (cancel.flags & IORING_CQE_F_MORE) == 0 &&
                finished.contains(cancel.value) && completion_index < finished.at(cancel.value),
                "cancellation CQE is not correlated to its settled original command");
        }
        require_fixture(prepared.size() == completed.size(), "prepared raw cookies lack terminal original CQEs");
        unsigned fetches[2] = {0, 0};
        for (const auto& [key, index] : prepared) {
            require_fixture(recovery_started < index, "raw command preceded successful START_RECOVERY");
            const auto completion = completed.find(key);
            require_fixture(completion != completed.end() && completion->second.first > index,
                            "raw completion missing or precedes preparation");
            if ((key.second & (uint64_t{1} << 63)) == 0) {
                ++fetches[key.first];
                require_fixture(completion->second.first > stop && completion->second.second == -ENODEV,
                                "parked FETCH lacks post-STOP original abort CQE");
            }
        }
        for (unsigned queue = 0; queue != 2; ++queue) {
            require_fixture(fetches[queue] == 4, "partial recovery did not prepare four FETCH per queue");
            require_fixture(issued[queue] < stop && bridges[queue] < stop &&
                            stop < exited[queue] && exited[queue] < closed[queue] && closed[queue] < remove,
                            "issuance/native retirement/owner exit/cdev close ordering invalid");
        }
        require_fixture(!replacement_log_->has(Kind::adopted), "unadopted case crossed adoption");
        report_["cookie_validation"] = "all real prepared cookies have one terminal original CQE; data FETCH aborted with ENODEV after authorized STOP";
    }

    std::string directory_, socket_;
    pid_t supervisor_ = -1, reader_ = -1, replacement_pid_ = -1;
    uint32_t id_ = UINT32_MAX;
    uint64_t generation_ = 0;
    bool created_ = false, crashed_ = false, end_failed_ = false;
    bool gate_opened_ = false, maintenance_ = false, supervisor_reaped_ = false, complete_ = false;
    std::vector<uint8_t> payload_;
    std::vector<PinnedNode> nodes_;
    std::unique_ptr<PinnedProcess> supervisor_pin_, original_, replacement_;
    std::unique_ptr<EventLog> original_log_, replacement_log_;
    Json report_;
};
} // namespace

TEST_CASE("integration: supervisor retains failed partial ublk recovery until explicit release",
          "[ublk][startup-kernel]") {
    if (const auto reason = obd::test::ublk_prerequisite_skip_reason(); !reason.empty()) SKIP(reason);
    bool maintenance = false;
    SECTION("explicit destroy retains management without supervisor shutdown") {}
    SECTION("supervisor shutdown keeps retained generation in maintenance") { maintenance = true; }
    KernelStartupFixture fixture;
    std::string failure;
    try { fixture.run(maintenance); }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "unknown fixture failure"; }
    if (!failure.empty()) fixture.attempt_supported_cleanup();
    const bool exported = fixture.finish_report(failure);
    INFO("preserved fixture evidence: " << fixture.directory());
    INFO("fixture failure: " << failure);
    CHECK(exported);
    CHECK(failure.empty());
}
