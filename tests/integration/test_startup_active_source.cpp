#include "startup_kernel_support.hpp"

namespace {
using namespace obd::test::startup_kernel;

// This controller owns only process identity handles and observations. The real
// supervisor/device retain all source, coroutine, queue and kernel ownership.
class ActiveSourceFixture {
public:
    ActiveSourceFixture() {
        auto name = (std::filesystem::temp_directory_path() / "obd-startup-source-XXXXXX").string();
        std::vector<char> writable(name.begin(), name.end());
        writable.push_back('\0');
        const char* directory = ::mkdtemp(writable.data());
        if (!directory) throw std::system_error(errno, std::generic_category(), "source fixture directory");
        directory_ = directory;
        socket_ = directory_ + "/s.sock";
        write_text(directory_ + "/adopted-source-v1", "successful END; real source buffer loan\n");
        write_text(directory_ + "/global.json", "{\"ublkConfig\":{\"enableRecovery\":true}}\n");
        payload_ = obd::test::pattern_bytes(256 * 1024, 117);
        const auto raw = obd::test::write_file(directory_ + "/raw", payload_);
        const int input = ::open(raw.c_str(), O_RDONLY | O_CLOEXEC);
        if (input < 0) throw std::system_error(errno, std::generic_category(), "source fixture image");
        try { obd::format::write_lsmt_single_layer(input, payload_.size(), directory_ + "/layer", {}); }
        catch (...) { ::close(input); throw; }
        ::close(input);
        write_text(directory_ + "/image.json", Json{{"lowers", Json::array({{{"file", directory_ + "/layer"}}})}}.dump());
    }
    ~ActiveSourceFixture() {
        if (reader_fd_ >= 0) ::close(reader_fd_);
        if (!complete_) std::fprintf(stderr,
            "retained active-source fixture: supervisor=%ld reader=%ld directory=%s\n",
            static_cast<long>(supervisor_), static_cast<long>(reader_), directory_.c_str());
        // No process kill, implicit STOP/DEL, unlink or automatic gate release.
    }
    const std::string& directory() const { return directory_; }
    void run() {
        start_supervisor();
        wait_for([&] {
            try { return call({{"cmd", "hello"}}).value("ok", false); }
            catch (...) { return false; }
        }, 5s, "source supervisor did not listen");
        const auto created = call({{"cmd", "create"}, {"id", "borrower"}, {"config", directory_ + "/image.json"}});
        remember("create", created);
        require_fixture(created.value("ok", false), "original source fixture create failed");
        auto status = call({{"cmd", "status"}, {"id", "borrower"}});
        require_fixture(status.at("state") == "ready", "original source fixture not ready");
        const auto original_pid = status.at("pid").get<pid_t>();
        const auto original_generation = status.at("startup_owner").at("generation").get<uint64_t>();
        original_log_ = std::make_unique<EventLog>(directory_, original_pid, original_generation);
        original_ = std::make_unique<PinnedProcess>(original_pid, original_log_->header().process_start_ticks);
        id_ = status.at("startup_owner").at("device_id").get<uint32_t>();
        obd::ublk::Ctrl inspect;
        const auto info = inspect.get_dev_info(id_);
        require_fixture(info.nr_hw_queues == 2 && info.queue_depth == 4, "source fixture geometry mismatch");
        constexpr uint64_t flags = UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE;
        require_fixture((info.flags & flags) == flags, "source fixture recovery flags not negotiated");
        const auto suffix = std::to_string(id_);
        bdev_ = "/dev/ublkb" + suffix;
        nodes_.emplace_back(bdev_);
        nodes_.emplace_back("/dev/ublkc" + suffix);
        nodes_.emplace_back("/sys/class/ublk-char/ublkc" + suffix);
        start_read(0);
        collect_read(true);
        original_->signal(SIGKILL); // Sole crash stimulus, only the proven ready original.
        wait_for([&] {
            status = call({{"cmd", "status"}, {"id", "borrower"}});
            return status.value("ok", false) && status.value("pid", pid_t{-1}) > 0 &&
                status.at("pid") != original_pid && status.contains("startup_owner") &&
                status.at("startup_owner").at("generation") != original_generation;
        }, 10s, "source replacement was not published");
        replacement_pid_ = status.at("pid").get<pid_t>();
        generation_ = status.at("startup_owner").at("generation").get<uint64_t>();
        wait_for([&] {
            const auto current = call({{"cmd", "status"}, {"id", "borrower"}});
            require_fixture(current.value("pid", pid_t{-1}) == replacement_pid_ &&
                current.contains("startup_owner") && current.at("startup_owner").at("generation") == generation_,
                "source replacement changed before initialized evidence");
            if (!EventLog::initialized(directory_, replacement_pid_, generation_)) return false;
            replacement_log_ = std::make_unique<EventLog>(directory_, replacement_pid_, generation_);
            replacement_ = std::make_unique<PinnedProcess>(replacement_pid_, replacement_log_->header().process_start_ticks);
            return true;
        }, 5s, "source replacement evidence not initialized");
        require_fixture(replacement_log_->header().adopted_source == 1 &&
                        replacement_log_->header().recovery == 1, "source mode/recovery identity mismatch");
        require_fixture(original_->reaped(), "source replacement preceded original actual reap");
        const int original_wait = ReapEvidence(directory_, supervisor_, report_)
                                      .wait_reap_evidence(*original_log_, "original");
        require_fixture(WIFSIGNALED(original_wait) && WTERMSIG(original_wait) == SIGKILL,
                        "source original did not have the intentional SIGKILL wait status");
        // The driver's dead-owner monitor may take five seconds. This is a
        // distinct START phase, not part of the later queue/setup budget.
        wait_for([&] { return replacement_log_->control_succeeded(UBLK_U_CMD_START_USER_RECOVERY); }, 35s,
                 "original START_RECOVERY did not succeed and settle");
        wait_for([&] { return replacement_log_->has(Kind::post_control_enter); }, 10s,
                 "successful END did not reach the pre-ready checkpoint");
        require_fixture(replacement_log_->control_succeeded(UBLK_U_CMD_END_USER_RECOVERY) &&
                        replacement_log_->has(Kind::adopted, UINT32_MAX, UBLK_U_CMD_END_USER_RECOVERY),
                        "checkpoint lacks real successful END and committed adoption");
        adopted_ = true;
        start_read(target_offset_);
        wait_for([&] {
            for (const auto& event : replacement_log_->events())
                if (event.kind == Kind::source_enter && event.offset == target_offset_ && event.length == 4096) return true;
            return false;
        }, 5s, "real block read did not enter the source gate");
        require_fixture(reader_ > 0, "real source borrower has no client process");
        assert_held(false);
        const auto destroyed = call({{"cmd", "destroy"}, {"id", "borrower"}});
        remember("destroy-with-borrower", destroyed);
        require_fixture(!destroyed.value("ok", false), "destroy falsely succeeded with live source loan");
        wait_for([&] {
            for (const auto& event : replacement_log_->events())
                if (event.kind == Kind::release_admitted && event.value == generation_ && event.release_sequence == 1) return true;
            return false;
        }, 5s, "operator release was not admitted before pre-ready handoff");
        release_admitted_ = true;
        replacement_log_->open_post_control();
        wait_for([&] { return replacement_log_->has(Kind::post_control_fail); }, 5s,
                 "local post-END failure did not execute");
        wait_for([&] {
            const auto current = call({{"cmd", "status"}, {"id", "borrower"}});
            return current.at("startup_owner").at("phase") == "retained";
        }, 5s, "adopted failure not observed while source loan is held");
        for (unsigned observation = 0; observation != 3; ++observation) {
            assert_held(true);
            std::this_thread::sleep_for(30ms);
        }
        replacement_log_->open_source();
        source_opened_ = true;
        collect_read(true, true);
        wait_for([&] { return replacement_log_->has(Kind::caller_terminal); }, 15s,
                 "adopted source generation did not reach caller terminal");
        validate_retirement();
        wait_for([&] { return replacement_->reaped(); }, 5s, "source generation not actually reaped");
        const int replacement_wait = ReapEvidence(directory_, supervisor_, report_)
                                         .wait_reap_evidence(*replacement_log_, "replacement");
        require_fixture(WIFEXITED(replacement_wait) && WEXITSTATUS(replacement_wait) == 0,
                        "source replacement did not exit normally with status zero");
        const auto removed = call({{"cmd", "destroy"}, {"id", "borrower"}});
        remember("destroy-after-reap", removed);
        require_fixture(removed.value("ok", false), "released source entry was not removed");
        require_fixture(call({{"cmd", "list"}}).at("devices").empty(), "source registry entry remains");
        supervisor_pin_->signal(SIGTERM);
        wait_supervisor();
        for (const auto& node : nodes_) require_fixture(node.removed(), "source fixture kernel node identity remains");
        size_t generations = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory_))
            if (entry.path().extension() == ".events") ++generations;
        require_fixture(generations == 2, "unexpected source fixture replacement spawned");
        complete_ = true;
    }
    void attempt_supported_cleanup() noexcept {
        if (complete_ || !adopted_ || !replacement_log_) return;
        // Successful END is already proven. A failed assertion can release
        // controllable test gates, but can never count as acceptance evidence.
        try { (void)call({{"cmd", "destroy"}, {"id", "borrower"}}); } catch (...) {}
        replacement_log_->open_post_control();
        replacement_log_->open_source();
        source_opened_ = true;
        try {
            if (reader_ > 0) collect_read(false);
            wait_for([&] { return replacement_log_->has(Kind::caller_terminal); }, 15s,
                     "cleanup retains actual source generation");
            (void)call({{"cmd", "destroy"}, {"id", "borrower"}});
            supervisor_pin_->signal(SIGTERM);
            wait_supervisor();
        } catch (...) {} // Retain remaining process/kernel owners for inspection.
    }
    bool finish_report(const std::string& failure) noexcept {
        try {
            report_["schema_version"] = 1;
            report_["mode"] = "adopted-source-v1";
            report_["failure"] = failure;
            report_["complete"] = complete_;
            report_["original_pid"] = original_ ? original_->pid() : -1;
            report_["original_generation"] = original_log_ ? original_log_->header().generation : 0;
            report_["original_start_ticks"] = original_log_ ? original_log_->header().process_start_ticks : 0;
            report_["supervisor_pid"] = supervisor_;
            report_["reader_pid"] = reader_;
            report_["replacement_pid"] = replacement_pid_;
            report_["replacement_generation"] = generation_;
            report_["replacement_start_ticks"] = replacement_log_ ? replacement_log_->header().process_start_ticks : 0;
            report_["device_id"] = id_;
            report_["adopted"] = adopted_;
            report_["release_admitted"] = release_admitted_;
            report_["source_opened"] = source_opened_;
            report_["post_control_open"] = replacement_log_ &&
                replacement_log_->header().post_control_open.load(std::memory_order_acquire) == 1;
            report_["directory"] = directory_;
            report_["export_errors"] = Json::array();
            report_["expected_exports"] = Json::array({"result.json", "supervisor.log"});
            report_["expected_raw_evidence"] = Json::array();
            bool exported = true;
            auto export_error = [&](const std::string& field, const std::string& error) {
                exported = false;
                report_[field] = error;
                report_["export_errors"].push_back({{"field", field}, {"error", error}});
            };
            if (!original_ || !replacement_ || !supervisor_pin_ || supervisor_ <= 0)
                export_error("identity_export_error", "missing pinned original/replacement/supervisor identity");
            const std::array<std::pair<const char*, const EventLog*>, 2> logs{{
                {"original", original_log_.get()}, {"replacement", replacement_log_.get()}}};
            for (const auto& [role, log] : logs) {
                const auto prefix = std::string(role);
                if (!log) {
                    export_error(prefix + "_export_error", "event ledger is missing");
                    export_error(prefix + "_reap_export_error", "reap identity cannot be matched without its ledger");
                    continue;
                }
                const auto& identity = log->header();
                const auto event_file = std::to_string(identity.pid) + "-" +
                                        std::to_string(identity.generation) + ".events";
                const auto reap_file = "child-" + std::to_string(identity.generation) + ".reap";
                report_["expected_exports"].push_back(event_file + ".json");
                report_["expected_exports"].push_back(reap_file + ".json");
                report_["expected_raw_evidence"].push_back(event_file);
                report_["expected_raw_evidence"].push_back(reap_file);
                try { log->dump(); }
                catch (const std::exception& error) { export_error(prefix + "_export_error", error.what()); }
                try { ReapEvidence(directory_, supervisor_, report_).export_reap_evidence(*log, role); }
                catch (const std::exception& error) { export_error(prefix + "_reap_export_error", error.what()); }
            }
            if (!std::filesystem::is_regular_file(directory_ + "/supervisor.log"))
                export_error("supervisor_export_error", "supervisor log is missing");
            report_["export_complete"] = exported;
            write_text(directory_ + "/result.json", report_.dump(2));
            return exported;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "active-source evidence export failed: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "active-source evidence export failed\n");
        }
        return false;
    }
private:
    Json call(const Json& command) const { return rpc(socket_, command); }
    void remember(const char* checkpoint, const Json& value) {
        report_["observations"].push_back({{"checkpoint", checkpoint}, {"record", value}});
    }
    void assert_held(bool failed) {
        const auto status = call({{"cmd", "status"}, {"id", "borrower"}});
        remember(failed ? "failed-with-borrower" : "checkpoint-with-borrower", status);
        require_fixture(status.at("pid") == replacement_pid_ && status.at("recoveries") == 1 &&
            status.at("startup_protected") == true && status.at("state") != "ready" &&
            status.at("startup_owner").at("generation") == generation_, "borrower lost protected startup identity");
        if (failed) require_fixture(status.at("startup_owner").at("phase") == "retained" &&
            status.at("startup_owner").at("reason_errno") == EIO, "local post-END failure not preserved");
        require_fixture(!replacement_->reaped(), "source owner disappeared while buffer borrowed");
        const auto list = call({{"cmd", "list"}});
        remember("held-list", list);
        require_fixture(list.at("devices").size() == 1 && list.at("devices").at(0).at("pid") == replacement_pid_,
                        "source fixture spawned competing replacement");
        const auto events = replacement_log_->events();
        std::set<uint32_t> borrowed_queues;
        for (const auto& loan : events) {
            if (loan.kind != Kind::source_enter) continue;
            bool matched = false;
            for (const auto& delivered : events) {
                if (delivered.kind == Kind::request_delivered && delivered.buffer == loan.buffer &&
                    delivered.offset == loan.offset && delivered.length == loan.length) {
                    borrowed_queues.insert(delivered.queue);
                    matched = true;
                }
            }
            require_fixture(matched, "source loan has no actual queue delivery identity");
        }
        for (const auto& event : events) {
            require_fixture(event.kind != Kind::bridge_destroyed || !borrowed_queues.contains(event.queue),
                            "borrowing bridge frame destroyed while source is held");
            require_fixture(event.kind != Kind::source_result && event.kind != Kind::source_scope_exit &&
                event.kind != Kind::source_destroyed &&
                event.kind != Kind::owner_exit && event.kind != Kind::cdev_closed && event.kind != Kind::caller_terminal,
                "resource retired while actual source buffer was held");
            require_fixture(event.kind != Kind::control_prepared ||
                (event.opcode != UBLK_U_CMD_STOP_DEV && event.opcode != UBLK_U_CMD_DEL_DEV),
                "STOP/DEL attempted while actual source buffer was held");
        }
    }
    void start_supervisor() {
        std::vector<std::string> args{OBD_TEST_SUPERVISOR_BIN, "--socket", socket_, "--global", directory_ + "/global.json",
            "--device-bin", OBD_TEST_DEVICE_BIN, "--ready-timeout", "45", "--stop-timeout", "1"};
        std::vector<char*> argv;
        for (auto& item : args) argv.push_back(item.data());
        argv.push_back(nullptr);
        std::vector<std::string> environment;
        for (char** item = environ; *item; ++item)
            if (!std::string_view(*item).starts_with("OBD_TEST_STARTUP_FIXTURE_DIR=")) environment.emplace_back(*item);
        environment.push_back("OBD_TEST_STARTUP_FIXTURE_DIR=" + directory_);
        std::vector<char*> envp;
        for (auto& item : environment) envp.push_back(item.data());
        envp.push_back(nullptr);
        posix_spawnattr_t attributes;
        posix_spawn_file_actions_t actions;
        require_fixture(::posix_spawnattr_init(&attributes) == 0, "source spawn attributes failed");
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            ::posix_spawnattr_destroy(&attributes);
            throw std::runtime_error("source spawn actions failed");
        }
        sigset_t empty, defaults;
        ::sigemptyset(&empty); ::sigemptyset(&defaults);
        ::sigaddset(&defaults, SIGTERM); ::sigaddset(&defaults, SIGINT); ::sigaddset(&defaults, SIGCHLD);
        int error = ::posix_spawnattr_setpgroup(&attributes, 0);
        if (!error) error = ::posix_spawnattr_setsigmask(&attributes, &empty);
        if (!error) error = ::posix_spawnattr_setsigdefault(&attributes, &defaults);
        if (!error) error = ::posix_spawnattr_setflags(&attributes,
            POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        if (!error) error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
            (directory_ + "/supervisor.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!error) error = ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!error) error = ::posix_spawn(&supervisor_, OBD_TEST_SUPERVISOR_BIN, &actions, &attributes, argv.data(), envp.data());
        ::posix_spawn_file_actions_destroy(&actions); ::posix_spawnattr_destroy(&attributes);
        if (error) throw std::system_error(error, std::generic_category(), "spawn source supervisor");
        supervisor_pin_ = std::make_unique<PinnedProcess>(supervisor_, process_start_ticks(supervisor_));
    }
    void wait_supervisor() {
        if (supervisor_reaped_) return;
        int status = 0;
        wait_for([&] {
            const auto result = ::waitpid(supervisor_, &status, WNOHANG);
            if (result == supervisor_) { supervisor_reaped_ = true; return true; }
            if (result < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category(), "reap source supervisor");
            return false;
        }, 5s, "source supervisor remains retained");
        require_fixture(WIFEXITED(status) && WEXITSTATUS(status) == 0, "source supervisor exited unsuccessfully");
    }
    struct ReadResult { int64_t count = -1; int error = 0; std::array<uint8_t, 4096> bytes{}; };
    void start_read(uint64_t offset) {
        require_fixture(reader_ <= 0 && reader_fd_ < 0, "a source reader is already live");
        void* memory = nullptr;
        if (::posix_memalign(&memory, 4096, 4096) != 0) throw std::bad_alloc();
        std::unique_ptr<void, decltype(&std::free)> buffer(memory, &std::free);
        int pipes[2];
        if (::pipe2(pipes, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "source read pipe");
        reader_ = ::fork();
        if (reader_ < 0) { ::close(pipes[0]); ::close(pipes[1]); throw std::runtime_error("source reader fork failed"); }
        if (reader_ == 0) {
            ::close(pipes[0]);
            ReadResult result;
            const int fd = ::open(bdev_.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
            result.count = fd < 0 ? -1 : ::pread(fd, memory, 4096, static_cast<off_t>(offset));
            result.error = result.count < 0 ? errno : 0;
            if (result.count == 4096) std::memcpy(result.bytes.data(), memory, 4096);
            if (fd >= 0) ::close(fd);
            const auto sent = ::write(pipes[1], &result, sizeof(result));
            ::close(pipes[1]);
            _exit(sent == static_cast<ssize_t>(sizeof(result)) ? 0 : 1);
        }
        ::close(pipes[1]);
        reader_fd_ = pipes[0];
        reader_offset_ = offset;
    }
    void collect_read(bool validate, bool permit_release_eio = false) {
        require_fixture(reader_ > 0 && reader_fd_ >= 0, "source reader missing");
        ReadResult result;
        size_t received = 0;
        while (received < sizeof(result)) {
            pollfd ready{reader_fd_, POLLIN, 0};
            require_fixture(::poll(&ready, 1, 5000) > 0, "actual block reader remains pending");
            const auto count = ::read(reader_fd_, reinterpret_cast<char*>(&result) + received, sizeof(result) - received);
            if (count < 0 && errno == EINTR) continue;
            require_fixture(count > 0, "actual block reader result incomplete");
            received += static_cast<size_t>(count);
        }
        ::close(reader_fd_); reader_fd_ = -1;
        int status = 0;
        wait_for([&] {
            const auto result_pid = ::waitpid(reader_, &status, WNOHANG);
            if (result_pid == reader_) return true;
            if (result_pid < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category(), "reap source reader");
            return false;
        }, 5s, "source reader not reaped");
        reader_ = -1;
        report_["reads"].push_back({{"offset", reader_offset_}, {"count", result.count}, {"errno", result.error}});
        if (validate) {
            require_fixture(WIFEXITED(status) && WEXITSTATUS(status) == 0, "block reader did not exit normally");
            const bool aborted = permit_release_eio && result.count == -1 && result.error == EIO;
            const bool correct = result.count == 4096 && result.error == 0 &&
                std::equal(result.bytes.begin(), result.bytes.end(), payload_.begin() + static_cast<ptrdiff_t>(reader_offset_));
            require_fixture(correct || aborted, "actual block read returned unexpected errno/short read/data");
            if (permit_release_eio) report_["replacement_client_outcome"] = aborted ? "EIO during authorized release" : "successful exact bytes";
        }
    }
    void validate_retirement();
    static constexpr uint64_t target_offset_ = 64 * 1024;
    std::string directory_, socket_, bdev_;
    pid_t supervisor_ = -1, replacement_pid_ = -1, reader_ = -1;
    int reader_fd_ = -1;
    uint64_t generation_ = 0, reader_offset_ = 0;
    uint32_t id_ = UINT32_MAX;
    bool adopted_ = false, release_admitted_ = false;
    bool source_opened_ = false, complete_ = false, supervisor_reaped_ = false;
    std::vector<uint8_t> payload_;
    std::vector<PinnedNode> nodes_;
    std::unique_ptr<PinnedProcess> supervisor_pin_, original_, replacement_;
    std::unique_ptr<EventLog> original_log_, replacement_log_;
    Json report_;
};

void ActiveSourceFixture::validate_retirement() {
    const auto events = replacement_log_->events();
    const size_t absent = events.size();
    size_t adoption = absent, checkpoint = absent, admitted = absent, failed = absent;
    size_t stop = absent, remove = absent, terminal = absent, destroyed = absent;
    unsigned stops = 0, deletions = 0, targets = 0, adoptions = 0;
    std::array<size_t, 2> bridges{absent, absent}, drained{absent, absent}, exited{absent, absent}, closed{absent, absent};
    using Key = std::pair<uint32_t, uint64_t>;
    std::map<Key, size_t> raw_prepared, raw_completed;
    std::map<uint64_t, size_t> prepared, originals, settled, cancels, cancel_completed;
    std::map<uint64_t, size_t> loans, results, scope_exits;
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        auto insert = [&](auto& map) {
            require_fixture(map.emplace(event.cookie, index).second, "control/source cookie reused or duplicated");
        };
        switch (event.kind) {
        case Kind::control_prepared:
            insert(prepared);
            if (event.opcode == UBLK_U_CMD_STOP_DEV) { stop = index; ++stops; }
            if (event.opcode == UBLK_U_CMD_DEL_DEV) { remove = index; ++deletions; }
            break;
        case Kind::control_original: insert(originals); break;
        case Kind::control_finished: insert(settled); break;
        case Kind::control_cancel_prepared: insert(cancels); break;
        case Kind::control_cancel: insert(cancel_completed); break;
        case Kind::source_enter: insert(loans); break;
        case Kind::source_result: insert(results); break;
        case Kind::source_scope_exit: insert(scope_exits); break;
        case Kind::source_destroyed:
            require_fixture(destroyed == absent, "source wrapper destroyed twice"); destroyed = index; break;
        case Kind::adopted:
            require_fixture(event.opcode == UBLK_U_CMD_END_USER_RECOVERY && event.result == 0, "wrong adoption event");
            adoption = index; ++adoptions; break;
        case Kind::post_control_enter: checkpoint = index; break;
        case Kind::post_control_fail:
            require_fixture(event.result == -EIO, "post-END local failure changed"); failed = index; break;
        case Kind::release_admitted:
            require_fixture(event.value == generation_ && event.release_sequence == 1, "wrong accepted release identity");
            admitted = index; break;
        case Kind::caller_terminal:
            terminal = index;
            require_fixture(event.flags == static_cast<uint32_t>(obd::ublk::StartupPhase::released) &&
                event.cookie == event.value && event.owner_flags == 1 && event.release_sequence == 1 &&
                event.opcode == static_cast<uint32_t>(obd::ublk::StartupRelease::released),
                "caller terminal did not retire the admitted release");
            break;
        default: break;
        }
        if (event.queue < 2) {
            if (event.kind == Kind::raw_prepared)
                require_fixture(raw_prepared.emplace(Key{event.queue, event.cookie}, index).second, "raw cookie reused");
            if (event.kind == Kind::raw_cqe) {
                require_fixture((event.flags & IORING_CQE_F_MORE) == 0, "raw CQE not terminal");
                require_fixture(raw_completed.emplace(Key{event.queue, event.cookie}, index).second, "duplicate raw original CQE");
            }
            if (event.kind == Kind::bridge_destroyed) bridges[event.queue] = std::min(bridges[event.queue], index);
            if (event.kind == Kind::sources_drained) drained[event.queue] = std::min(drained[event.queue], index);
            if (event.kind == Kind::owner_exit) {
                require_fixture(event.value == 1, "owner exit has no actual exit proof"); exited[event.queue] = index;
            }
            if (event.kind == Kind::cdev_closed) {
                require_fixture(event.result == 0, "source queue cdev close failed"); closed[event.queue] = index;
            }
        }
    }
    require_fixture(adoptions == 1 && adoption < checkpoint && checkpoint < admitted && admitted < failed &&
        failed < stop && stops == 1 && deletions == 1 && stop < remove && remove < destroyed && destroyed < terminal,
        "adoption/checkpoint/release/failure/STOP/DEL/source/caller ordering invalid");
    require_fixture(prepared.size() == originals.size() && prepared.size() == settled.size(),
                    "control originals/settlements missing");
    size_t start_success = absent, end_success = absent;
    unsigned start_successes = 0, end_successes = 0;
    for (const auto& [cookie, index] : prepared) {
        require_fixture(originals.contains(cookie) && settled.contains(cookie), "control cookie lacks original/settlement");
        const auto original_index = originals.at(cookie), settle_index = settled.at(cookie);
        const auto& original = events[original_index];
        require_fixture(index < original_index && original_index < settle_index &&
            events[index].opcode == original.opcode && events[settle_index].opcode == original.opcode &&
            events[settle_index].result == original.result && (original.flags & IORING_CQE_F_MORE) == 0,
            "control preparation/original/settlement disagree");
        if (original.opcode == UBLK_U_CMD_START_USER_RECOVERY) {
            if (original.result == 0) { start_success = original_index; ++start_successes; }
            else require_fixture(original.result == -EBUSY && start_successes == 0, "unexpected START_RECOVERY original failure");
        }
        if (original.opcode == UBLK_U_CMD_END_USER_RECOVERY) {
            if (original.result == 0) { end_success = original_index; ++end_successes; }
            else require_fixture(original.result == -EBUSY && end_successes == 0, "unexpected END original failure");
        }
        if (original.opcode == UBLK_U_CMD_STOP_DEV)
            require_fixture(original.result == 0, "STOP original did not succeed");
        if (original.opcode == UBLK_U_CMD_DEL_DEV)
            require_fixture(original.result == 0 || original.result == -ENODEV, "DEL original did not retire device");
    }
    require_fixture(start_successes == 1 && end_successes == 1 && start_success < end_success && end_success < adoption,
                    "successful START/END originals missing or adoption precedes original success");
    require_fixture(cancels.size() == cancel_completed.size(), "control cancel lacks its own CQE");
    for (const auto& [cookie, index] : cancels) {
        require_fixture(cancel_completed.contains(cookie), "missing cancellation CQE");
        const auto completion = cancel_completed.at(cookie);
        const auto& event = events[completion];
        require_fixture(index < completion && event.value == events[index].value && event.opcode == events[index].opcode &&
            (event.flags & IORING_CQE_F_MORE) == 0 && settled.contains(event.value) && completion < settled.at(event.value),
            "cancellation not correlated with its original settlement");
    }
    require_fixture(raw_prepared.size() == raw_completed.size(), "raw originals not completely retired");
    for (const auto& [key, index] : raw_prepared) {
        require_fixture(raw_completed.contains(key) && index < raw_completed.at(key) && start_success < index,
                        "raw original completion missing or preparation predates START success");
        const auto completion_index = raw_completed.at(key);
        const auto& completion = events[completion_index];
        require_fixture(key.first < 2 && completion_index < exited[key.first],
                        "raw original completion did not precede its queue owner exit");
        if ((key.second & (uint64_t{1} << 63)) == 0) {
            require_fixture(completion.result >= 0 || completion.result == -ENODEV,
                            "unexpected original FETCH/COMMIT result");
            if (completion.result == -ENODEV)
                require_fixture(stop < completion_index, "aborted data original preceded authorized STOP");
        }
    }
    require_fixture(!loans.empty() && loans.size() == results.size() && loans.size() == scope_exits.size(),
                    "source loans did not all complete and leave their scopes");
    for (const auto& [cookie, entered] : loans) {
        require_fixture(results.contains(cookie) && scope_exits.contains(cookie), "source loan missing result/scope exit");
        const auto result_index = results.at(cookie), exit_index = scope_exits.at(cookie);
        const auto& loan = events[entered];
        const auto& result = events[result_index];
        require_fixture(entered < result_index && failed < result_index && result_index < exit_index &&
            result.buffer == loan.buffer && result.offset == loan.offset && result.length == loan.length &&
            result.result == static_cast<int32_t>(loan.length) && loan.offset <= payload_.size() &&
            loan.length <= payload_.size() - loan.offset, "source delegate or live-loan ordering mismatch");
        uint64_t checksum = 14695981039346656037ULL;
        for (uint64_t index = loan.offset; index < loan.offset + loan.length; ++index) {
            checksum ^= payload_[static_cast<size_t>(index)]; checksum *= 1099511628211ULL;
        }
        require_fixture(result.value == checksum, "real source delegate bytes mismatch");
        size_t matching_deliveries = 0;
        for (size_t delivery_index = 0; delivery_index < entered; ++delivery_index) {
            const auto& delivery = events[delivery_index];
            if (delivery.kind != Kind::request_delivered || delivery.buffer != loan.buffer ||
                delivery.offset != loan.offset || delivery.length != loan.length) continue;
            ++matching_deliveries;
            const Key key{delivery.queue, delivery.cookie};
            require_fixture(delivery.queue < 2 && delivery.tag < 4 && delivery.opcode == UBLK_IO_OP_READ &&
                raw_completed.contains(key) && raw_completed.at(key) < delivery_index &&
                events[raw_completed.at(key)].result >= 0 && exit_index < bridges[delivery.queue],
                "loan lacks real delivered request/original CQE/native bridge retirement");
            report_["loans"].push_back({{"loan", cookie}, {"queue", delivery.queue}, {"tag", delivery.tag},
                {"buffer", loan.buffer}, {"offset", loan.offset}, {"length", loan.length},
                {"original_cookie", delivery.cookie}, {"delegate_result", result.result}, {"checksum", result.value}});
        }
        require_fixture(matching_deliveries == 1, "source loan has missing/ambiguous queue origin");
        if (loan.offset == target_offset_ && loan.length == 4096) {
            ++targets;
            require_fixture(checkpoint < entered && entered < admitted,
                            "target borrower did not enter between checkpoint and release admission");
        }
    }
    require_fixture(targets == 1, "target block request missing or duplicated");
    for (unsigned queue = 0; queue != 2; ++queue)
        require_fixture(bridges[queue] < drained[queue] && drained[queue] < stop &&
            stop < exited[queue] && exited[queue] < closed[queue] && closed[queue] < remove,
            "native bridge/source drain/STOP/owner/cdev/DEL order invalid");
    report_["validation"] = "real queue-origin buffer loans delegated exact bytes; all native bridges destroyed before STOP; raw/control originals retired; sequence 1 released";
}
} // namespace

TEST_CASE("integration: adopted ublk recovery drains source frames before explicit release", "[ublk][startup-kernel-source]") {
    if (const auto reason = obd::test::ublk_prerequisite_skip_reason(); !reason.empty()) SKIP(reason);
    ActiveSourceFixture fixture;
    std::string failure;
    try { fixture.run(); }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "unknown active-source fixture failure"; }
    if (!failure.empty()) fixture.attempt_supported_cleanup();
    const bool exported = fixture.finish_report(failure);
    INFO("preserved active-source evidence: " << fixture.directory());
    CHECK(exported);
    CHECK(failure.empty());
}
