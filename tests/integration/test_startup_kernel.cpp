#include "startup_kernel_support.hpp"

namespace {
using namespace obd::test::startup_kernel;

class KernelStartupFixture {
public:
    explicit KernelStartupFixture(bool report_refusal = false) : report_refusal_(report_refusal) {
        const auto prefix = report_refusal_ ? "obd-startup-report-retry-XXXXXX" : "obd-startup-kernel-XXXXXX";
        auto pattern = (std::filesystem::temp_directory_path() / prefix).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* made = ::mkdtemp(writable.data());
        if (!made) throw std::system_error(errno, std::generic_category(), "fixture directory");
        directory_ = made;
        socket_ = directory_ + "/s.sock";
        write_text(directory_ + (report_refusal_ ? "/report-refusal-v1" : "/partial-recovery-v1"),
                   "two queues; depth four; recovery queue one gate\n");
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
        if (report_refusal_) {
            require_fixture(replacement_log_->header().report_refusal == 1, "report/refusal mode not enabled");
            wait_for([&] {
                return replacement_log_->has(Kind::failure_report_throw) &&
                    replacement_log_->has(Kind::failure_report_caught) &&
                    replacement_log_->has(Kind::failure_report_constructed);
            }, 5s, "real caller failure report did not throw, catch and reconstruct");
            uint64_t constructed_revision = 0;
            for (const auto& event : replacement_log_->events()) {
                if (event.kind == Kind::failure_report_constructed) constructed_revision = event.value;
                if (event.kind == Kind::control_finished && event.opcode == UBLK_U_CMD_END_USER_RECOVERY &&
                    event.result < 0 && (event.value == ETIMEDOUT || event.value == ECANCELED))
                    failure_reason_ = static_cast<int>(event.value);
            }
            require_fixture(constructed_revision != 0 && failure_reason_ != 0, "missing report/END failure identity");
            // Require the supervisor to receive this recovered revision (or a
            // later one); a local construction event is not wire evidence.
            wait_for([&] {
                status = call({{"cmd", "status"}, {"id", "partial"}});
                const auto& owner = status.at("startup_owner");
                require_fixture(status.at("pid") == replacement_pid_ && owner.at("generation") == generation_,
                                "report exception replaced its protected owner");
                return owner.at("revision").get<uint64_t>() >= constructed_revision &&
                       owner.at("control_pending") == false;
            }, 5s, "supervisor did not observe the recovered failure report revision");
            check_retained(status);
            require_fixture(status.at("startup_owner").at("reason_errno") == failure_reason_,
                            "report fault erased the original END failure reason");
            remember("status-after-report-exception", status);
        }
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
        if (report_refusal_) {
            exercise_report_refusal_retry();
        } else {
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
        }
        wait_for([&] { return replacement_log_->has(Kind::caller_terminal); }, 15s,
                 "released generation did not retire to caller terminal");
        validate_retirement();
        wait_for([&] { return replacement_->reaped(); }, 5s, "terminal replacement was not actually reaped");
        const int replacement_wait = wait_reap_evidence(*replacement_log_, "replacement");
        require_fixture(WIFEXITED(replacement_wait) && WEXITSTATUS(replacement_wait) == 0,
                        "retired replacement did not exit normally with status zero");
        if (!maintenance_) {
            if (!retry_removed_) {
                const auto destroyed = call({{"cmd", "destroy"}, {"id", "partial"}});
                remember("destroy-after-retirement", destroyed);
                require_fixture(destroyed.value("ok", false), "reaped released entry was not removed");
            }
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
            report_["mode"] = report_refusal_ ? "report-refusal-v1" : "partial-recovery-v1";
            report_["schema_version"] = 1;
            report_["report_refusal"] = report_refusal_;
            report_["expected_release_sequence"] = report_refusal_ ? 2 : 1;
            report_["failure_reason"] = failure_reason_;
            report_["export_errors"] = Json::array();
            report_["expected_exports"] = Json::array({"result.json", "supervisor.log"});
            report_["expected_raw_evidence"] = Json::array();
            for (const auto* log : {original_log_.get(), replacement_log_.get()}) {
                if (!log) continue;
                const auto& identity = log->header();
                const auto events = std::to_string(identity.pid) + "-" + std::to_string(identity.generation) + ".events";
                const auto reap = "child-" + std::to_string(identity.generation) + ".reap";
                report_["expected_exports"].push_back(events + ".json");
                report_["expected_exports"].push_back(reap + ".json");
                report_["expected_raw_evidence"].push_back(events);
                report_["expected_raw_evidence"].push_back(reap);
            }
            if (!original_log_ || !replacement_log_)
                report_["export_errors"].push_back("missing original/replacement event ledger");
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
                    report_["export_errors"].push_back(Json{{"field", "original_export_error"}, {"error", error.what()}});
                }
                try { export_reap_evidence(*original_log_, "original"); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["original_reap_export_error"] = error.what();
                    report_["export_errors"].push_back(Json{{"field", "original_reap_export_error"}, {"error", error.what()}});
                }
            }
            if (replacement_log_) {
                try { replacement_log_->dump(); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["replacement_export_error"] = error.what();
                    report_["export_errors"].push_back(Json{{"field", "replacement_export_error"}, {"error", error.what()}});
                }
                try { export_reap_evidence(*replacement_log_, "replacement"); }
                catch (const std::exception& error) {
                    exported = false;
                    report_["replacement_reap_export_error"] = error.what();
                    report_["export_errors"].push_back(Json{{"field", "replacement_reap_export_error"}, {"error", error.what()}});
                }
            }
            if (!original_ || !replacement_ || !supervisor_pin_ ||
                !std::filesystem::is_regular_file(directory_ + "/supervisor.log")) {
                exported = false;
                report_["export_errors"].push_back("missing pinned identity or supervisor log");
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
                    wait_for([&] {
                        if (replacement_log_->has(Kind::caller_terminal)) return true;
                        if (report_refusal_) {
                            const auto status = call({{"cmd", "status"}, {"id", "partial"}});
                            if (status.value("ok", false) && status.contains("startup_owner") &&
                                status.at("startup_owner").at("release") == "refused")
                                (void)call({{"cmd", "destroy"}, {"id", "partial"}});
                        }
                        return false;
                    }, 15s, "cleanup still owns kernel commands");
                    (void)call({{"cmd", "destroy"}, {"id", "partial"}});
                }
            }
            if (supervisor_pin_) supervisor_pin_->signal(SIGTERM);
            wait_supervisor();
        } catch (...) {} // Preserve the process, kernel resource and evidence.
    }
private:
    void exercise_report_refusal_retry() {
        // END has failed and settled; publishing the held real batch cannot
        // turn this into a late-success/adopted scenario.
        replacement_log_->open_queue_one();
        gate_opened_ = true;
        wait_for([&] { return replacement_log_->has(Kind::initial_issued, 1); }, 5s,
                 "second queue did not issue before local release setup failure");
        require_no_stop_or_delete();
        const auto refused = call({{"cmd", "destroy"}, {"id", "partial"}});
        remember("destroy-local-refusal", refused);
        require_fixture(!refused.value("ok", false), "first local refusal falsely reported destroy success");
        Json status;
        wait_for([&] {
            status = call({{"cmd", "status"}, {"id", "partial"}});
            const auto& owner = status.at("startup_owner");
            return owner.at("release_sequence") == 1 && owner.at("release") == "refused";
        }, 5s, "first release did not settle as refused sequence one");
        for (unsigned observation = 0; observation != 3; ++observation) {
            check_retained(status);
            const auto& owner = status.at("startup_owner");
            require_fixture(owner.at("release_sequence") == 1 && owner.at("release") == "refused" &&
                owner.at("release_errno") == ENOMEM && owner.at("reason_errno") == failure_reason_,
                "settled local refusal changed sequence, errno or original END failure");
            require_no_stop_or_delete();
            require_fixture(!replacement_log_->has(Kind::caller_terminal) &&
                            !replacement_log_->has(Kind::cdev_closed), "refused attempt falsely retired resources");
            const auto listed = call({{"cmd", "list"}});
            remember("refused-list", listed);
            require_fixture(listed.at("devices").size() == 1 &&
                            listed.at("devices").at(0).at("pid") == replacement_pid_ &&
                            listed.at("devices").at(0).at("recoveries") == 1,
                            "refusal replaced or removed the protected generation");
            remember("refused-status", status);
            std::this_thread::sleep_for(30ms);
            status = call({{"cmd", "status"}, {"id", "partial"}});
        }
        const auto retry = call({{"cmd", "destroy"}, {"id", "partial"}});
        remember("destroy-fresh-retry", retry);
        retry_removed_ = retry.value("ok", false);
        wait_for([&] {
            for (const auto& event : replacement_log_->events())
                if (event.kind == Kind::release_admitted && event.value == generation_ &&
                    event.release_sequence == 2) return true;
            return false;
        }, 5s, "fresh retry did not admit sequence two for the same generation");
    }
    int wait_reap_evidence(const EventLog& log, const char* role) {
        return ReapEvidence(directory_, supervisor_, report_).wait_reap_evidence(log, role);
    }
    void export_reap_evidence(const EventLog& log, const char* role) {
        ReapEvidence(directory_, supervisor_, report_).export_reap_evidence(log, role);
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
                                event.release_sequence == (report_refusal_ ? 2U : 1U),
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
            // ADR-0023 keeps the fixed-owner coroutine alive to collect every
            // post-STOP raw CQE. bridge_destroyed is the historical event name
            // for destruction of that final owner frame.
            require_fixture(issued[queue] < stop && stop < exited[queue] &&
                            exited[queue] < bridges[queue] &&
                            bridges[queue] < closed[queue] && closed[queue] < remove,
                            "issuance/STOP/owner exit/frame destruction/cdev close ordering invalid");
        }
        require_fixture(!replacement_log_->has(Kind::adopted), "unadopted case crossed adoption");
        if (report_refusal_) {
            size_t thrown = events.size(), caught = events.size(), constructed = events.size();
            size_t setup = events.size(), admitted_one = events.size(), admitted_two = events.size();
            unsigned throws = 0, catches = 0, constructions = 0, setups = 0, admissions = 0;
            for (size_t index = 0; index < events.size(); ++index) {
                const auto& event = events[index];
                if (event.kind == Kind::failure_report_throw) { thrown = index; ++throws; }
                if (event.kind == Kind::failure_report_caught) { caught = index; ++catches; }
                if (event.kind == Kind::failure_report_constructed) { constructed = index; ++constructions; }
                if (event.kind == Kind::release_setup_throw) {
                    require_fixture(event.result == -ENOMEM && event.release_sequence == 1,
                                    "local setup fault has wrong errno or sequence");
                    setup = index; ++setups;
                }
                if (event.kind == Kind::release_admitted) {
                    require_fixture(event.value == generation_, "release admitted for wrong generation");
                    ++admissions;
                    if (event.release_sequence == 1) admitted_one = index;
                    else if (event.release_sequence == 2) admitted_two = index;
                    else throw std::runtime_error("unexpected additional release sequence");
                }
            }
            // release_admitted is observed after the mailbox CAS: another
            // thread may enter setup/STOP before that diagnostic record lands.
            // Refused wire status and final sequence 2 prove the protocol order.
            require_fixture(throws == 1 && catches == 1 && constructions == 1 && setups == 1 && admissions == 2 &&
                thrown < caught && caught < constructed && constructed < admitted_one && constructed < setup &&
                admitted_one < admitted_two && issued[1] < setup && setup < admitted_two && setup < stop,
                "report containment/local refusal/fresh admission ordering or count invalid");
            report_["fault_validation"] = "one real caller report exception contained; sequence 1 refused ENOMEM before STOP setup; fresh sequence 2 retired actual resources";
        }
        report_["cookie_validation"] = "all real prepared cookies have one terminal original CQE; data FETCH aborted with ENODEV after authorized STOP";
    }

    bool report_refusal_ = false, retry_removed_ = false;
    int failure_reason_ = 0;
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

TEST_CASE("integration: retained ublk caller survives report failure and retries refused release",
          "[ublk][startup-kernel-report-retry]") {
    if (const auto reason = obd::test::ublk_prerequisite_skip_reason(); !reason.empty()) SKIP(reason);
    KernelStartupFixture fixture(true);
    std::string failure;
    try { fixture.run(false); }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "unknown reporting/refusal fixture failure"; }
    if (!failure.empty()) fixture.attempt_supported_cleanup();
    const bool exported = fixture.finish_report(failure);
    INFO("preserved report/refusal evidence: " << fixture.directory());
    INFO("fixture failure: " << failure);
    CHECK(exported);
    CHECK(failure.empty());
}
