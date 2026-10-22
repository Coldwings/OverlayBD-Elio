// Shared observation/controller helpers for the owned real-kernel fixtures.
#pragma once
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

namespace obd::test::startup_kernel {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Kind = obd::ublk::detail::StartupTestKind;
using Event = obd::ublk::detail::StartupTestEvent;
using Log = obd::ublk::detail::StartupFixtureLog;

inline void require_fixture(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
inline void write_text(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
    file.close();
    if (!file) throw std::runtime_error("cannot write fixture file: " + path);
}
inline uint64_t process_start_ticks(pid_t pid) {
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
    void open_post_control() { log_->post_control_open.store(1, std::memory_order_release); }
    void open_source() { log_->source_open.store(1, std::memory_order_release); }
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
        document["adopted_source"] = log_->adopted_source;
        document["report_refusal"] = log_->report_refusal;
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
                {"owner_flags", event.owner_flags}, {"release_sequence", event.release_sequence},
                {"buffer", event.buffer}, {"offset", event.offset}, {"length", event.length}, {"tag", event.tag}});
        }
        document["committed_count"] = records.size();
        document["queue_one_open"] = log_->queue_one_open.load();
        document["post_control_open"] = log_->post_control_open.load();
        document["source_open"] = log_->source_open.load();
        write_text(path_ + ".json", document.dump(2));
    }
private:
    std::string path_;
    Log* log_ = nullptr;
};

inline Json rpc(const std::string& path, const Json& command) {
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

// Reads only the supervisor-owned successful waitpid witness, correlated with
// a previously pinned live event ledger. It never signals or collects a PID.
class ReapEvidence {
public:
    ReapEvidence(const std::string& directory, pid_t supervisor, Json& report)
        : directory_(directory), supervisor_(supervisor), report_(report) {}
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
private:
    const std::string& directory_;
    pid_t supervisor_;
    Json& report_;
};
} // namespace obd::test::startup_kernel
