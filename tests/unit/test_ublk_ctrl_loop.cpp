// These tests execute Ctrl's production command loop. Only the ring boundary
// is scripted; completion classification, cancellation and payload lifetime
// remain production code. No kernel device or io_uring instance is opened.
#include "ublk/ctrl.hpp"
#include "common/errors.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

namespace {
using namespace obd::ublk;

enum class Scenario {
    cancel_first,
    original_first,
    cancel_not_found,
    failed_submit,
    short_submit,
    unknown_cqe,
    duplicate_cqe,
    multishot_cqe,
    no_sqe,
};

struct Sqe128 {
    io_uring_sqe sqe{};
    std::array<unsigned char, sizeof(io_uring_sqe)> extension{};
};
static_assert(sizeof(Sqe128) == 128);

class Script final : public detail::CtrlTransport {
public:
    Script(Scenario which, int diagnostic_fd) : scenario(which), diagnostic_fd_(diagnostic_fd) {}

    void check(bool condition, int line) const noexcept {
        if (condition) return;
        // Async-safe diagnostic from the isolated fixture; do not call Catch
        // assertions or throw through the noexcept control transport.
        while (::write(diagnostic_fd_, &line, sizeof(line)) < 0 && errno == EINTR) {}
        ::_exit(100);
    }

#define EXPECT_SCRIPT(expression) check((expression), __LINE__)

    io_uring_sqe* get_sqe() noexcept override {
        if (scenario == Scenario::no_sqe) return nullptr;
        EXPECT_SCRIPT(prepared_ < sqes_.size());
        return &sqes_[prepared_++].sqe;
    }
    unsigned sq_ready() noexcept override { return prepared_ - submitted_; }

    int submit() noexcept override {
        ++submits;
        if (scenario == Scenario::short_submit && submits == 1) return 0;
        const unsigned limit = scenario == Scenario::short_submit && submits == 2
            ? submitted_ + 1 : prepared_;
        const unsigned count = limit - submitted_;
        while (submitted_ < limit) {
            const auto& sqe = sqes_[submitted_++].sqe;
            if (!original_) {
                EXPECT_SCRIPT(sqe.opcode == IORING_OP_URING_CMD);
                EXPECT_SCRIPT(sqe.cmd_op == UBLK_U_CMD_ADD_DEV);
                original_ = sqe.user_data;
                std::memcpy(&command_, sqe.cmd, sizeof(command_));
                EXPECT_SCRIPT(command_.len == sizeof(ublksrv_ctrl_dev_info));
                EXPECT_SCRIPT(command_.addr != 0);
                EXPECT_SCRIPT(command_.queue_id == UINT16_MAX);
                EXPECT_SCRIPT(payload()->nr_hw_queues == 3);
                EXPECT_SCRIPT(payload()->queue_depth == 17);
                EXPECT_SCRIPT(payload()->dev_id == UINT32_MAX);
                payload()->dev_id = 73;
            } else {
                EXPECT_SCRIPT(sqe.opcode == IORING_OP_ASYNC_CANCEL);
                EXPECT_SCRIPT(sqe.addr == original_);
                EXPECT_SCRIPT(sqe.user_data != original_);
                EXPECT_SCRIPT(!cancellation_);
                cancellation_ = sqe.user_data;
            }
        }
        // An ambiguous submission failure may follow publication. The actual
        // original CQE, rather than this errno, must settle the payload.
        if (scenario == Scenario::failed_submit && submits == 1) return -EIO;
        return static_cast<int>(count);
    }

    int enter() noexcept override {
        ++iteration;
        EXPECT_SCRIPT(iteration < 12);
        EXPECT_SCRIPT(read_ == written_);
        read_ = written_ = 0;
        if (scenario == Scenario::short_submit) {
            if (iteration == 2) {
                EXPECT_SCRIPT(original_ != 0);
                EXPECT_SCRIPT(cancellation_ == 0);
                EXPECT_SCRIPT(sq_ready() == 1);
            }
            if (iteration == 3) enqueue(cancellation_, -ENOENT);
            if (iteration == 5) enqueue(original_, 0);
        } else if (iteration == 2) {
            EXPECT_SCRIPT(original_ != 0 && cancellation_ != 0);
            switch (scenario) {
            case Scenario::original_first: enqueue(original_, 0); break;
            case Scenario::cancel_not_found: enqueue(cancellation_, -ENOENT); break;
            case Scenario::unknown_cqe:
                enqueue(cancellation_, 0);
                enqueue(original_ + 100, 0);
                break;
            case Scenario::duplicate_cqe:
                enqueue(original_, 0);
                enqueue(original_, 0);
                enqueue(cancellation_, 0);
                break;
            case Scenario::multishot_cqe:
                enqueue(original_, 0, IORING_CQE_F_MORE);
                enqueue(cancellation_, 0);
                break;
            default: enqueue(cancellation_, 0); break;
            }
        } else if (iteration == 4) {
            if (scenario == Scenario::original_first) enqueue(cancellation_, -ENOENT);
            else if (scenario != Scenario::duplicate_cqe)
                enqueue(original_, scenario == Scenario::cancel_not_found ||
                                   scenario == Scenario::failed_submit ? -ECANCELED : 0);
        }
        return 0;
    }

    int peek_cqe(io_uring_cqe** cqe) noexcept override {
        *cqe = read_ < written_ ? &cqes_[read_] : nullptr;
        return *cqe ? 0 : -EAGAIN;
    }
    void cqe_seen(io_uring_cqe* cqe) noexcept override {
        EXPECT_SCRIPT(read_ < written_);
        EXPECT_SCRIPT(cqe == &cqes_[read_]);
        ++read_;
    }

    void idle() noexcept override {
        EXPECT_SCRIPT(ctrl != nullptr && ctrl->command_pending());
        EXPECT_SCRIPT(progress.pending);
        EXPECT_SCRIPT(final_reports == 0);
        // The supplied address is still the actual add_dev stack payload.
        // Mutating it before retirement and checking the returned ID prevents
        // a detached copy or a prematurely returned command from passing.
        if (original_) {
            EXPECT_SCRIPT(payload()->dev_id == 73);
            EXPECT_SCRIPT(payload()->queue_depth == 17);
        }
        if (iteration == 2 && scenario == Scenario::original_first) {
            EXPECT_SCRIPT(progress.original_known && !progress.cancel_known);
            EXPECT_SCRIPT(ctrl->added_dev_id() == 73);
            EXPECT_SCRIPT(completions == 1);
        }
        if (iteration == 2 && (scenario == Scenario::cancel_first ||
                              scenario == Scenario::cancel_not_found ||
                              scenario == Scenario::failed_submit)) {
            EXPECT_SCRIPT(progress.cancel_known && !progress.original_known);
            EXPECT_SCRIPT(ctrl->added_dev_id() == -1);
            EXPECT_SCRIPT(completions == 0);
        }
        if (poisoned() && iteration == 6) {
            // Preserve the earlier cancellation reason; the decisive poison
            // evidence is that both known CQEs still cannot release ownership.
            EXPECT_SCRIPT(progress.reason_errno == ECANCELED);
            EXPECT_SCRIPT(progress.original_known && progress.cancel_known);
            EXPECT_SCRIPT(completions == 1);
            EXPECT_SCRIPT(ctrl->added_dev_id() == 73);
            // There is deliberately no safe return/unwind after an unknown
            // completion. Exit only this synthetic, kernel-free subprocess.
            ::_exit(0);
        }
    }

    static int cancel(void* context, uint32_t opcode) noexcept {
        auto& self = *static_cast<Script*>(context);
        self.EXPECT_SCRIPT(opcode == UBLK_U_CMD_ADD_DEV);
        return ECANCELED;
    }
    static void completed(void* context, uint32_t opcode, int result) noexcept {
        auto& self = *static_cast<Script*>(context);
        self.EXPECT_SCRIPT(opcode == UBLK_U_CMD_ADD_DEV);
        self.EXPECT_SCRIPT(self.ctrl->command_pending());
        self.EXPECT_SCRIPT(++self.completions == 1);
        // The ownership commit must precede this observer notification.
        self.EXPECT_SCRIPT(result != 0 || self.ctrl->added_dev_id() == 73);
    }
    static void reported(void* context, const CtrlProgress& state) noexcept {
        auto& self = *static_cast<Script*>(context);
        self.progress = state;
        if (!state.pending) ++self.final_reports;
    }

    bool poisoned() const noexcept {
        return scenario == Scenario::unknown_cqe ||
               scenario == Scenario::duplicate_cqe ||
               scenario == Scenario::multishot_cqe;
    }

    Scenario scenario;
    Ctrl* ctrl = nullptr;
    CtrlProgress progress;
    int iteration = 0;
    int submits = 0;
    int completions = 0;
    int final_reports = 0;

private:
    ublksrv_ctrl_dev_info* payload() const noexcept {
        return reinterpret_cast<ublksrv_ctrl_dev_info*>(command_.addr);
    }
    void enqueue(uint64_t cookie, int result, unsigned flags = 0) noexcept {
        EXPECT_SCRIPT(cookie != 0 && written_ < cqes_.size());
        auto& cqe = cqes_[written_++];
        cqe.user_data = cookie;
        cqe.res = result;
        cqe.flags = flags;
    }
    int diagnostic_fd_;
    std::array<Sqe128, 2> sqes_{};
    std::array<io_uring_cqe, 3> cqes_{};
    unsigned prepared_ = 0;
    unsigned submitted_ = 0;
    unsigned read_ = 0;
    unsigned written_ = 0;
    uint64_t original_ = 0;
    uint64_t cancellation_ = 0;
    ublksrv_ctrl_cmd command_{};
};

void exercise(Scenario scenario, int diagnostic_fd) {
    Script script(scenario, diagnostic_fd);
    {
        Ctrl ctrl(script);
        ctrl.disable_destructor_cleanup();
        script.ctrl = &ctrl;
        ctrl.set_observer({&script, &Script::cancel, &Script::completed, &Script::reported});
        if (scenario == Scenario::no_sqe) {
            const int result = ctrl.stop_dev_result(73);
            script.EXPECT_SCRIPT(result == -EBUSY);
            script.EXPECT_SCRIPT(!ctrl.command_pending());
            script.EXPECT_SCRIPT(script.progress.original_known);
            script.EXPECT_SCRIPT(script.final_reports == 1);
            script.EXPECT_SCRIPT(script.submits == 0);
        } else {
            DeviceParams params;
            params.nr_queues = 3;
            params.queue_depth = 17;
            int error = 0;
            uint32_t id = UINT32_MAX;
            try { id = ctrl.add_dev(params); }
            catch (const obd::error& e) { error = e.errno_value(); }
            script.EXPECT_SCRIPT(!script.poisoned());
            script.EXPECT_SCRIPT(!ctrl.command_pending());
            script.EXPECT_SCRIPT(script.final_reports == 1);
            script.EXPECT_SCRIPT(script.completions == 1);
            script.EXPECT_SCRIPT(script.progress.original_known && script.progress.cancel_known);
            if (scenario == Scenario::failed_submit || scenario == Scenario::cancel_not_found) {
                script.EXPECT_SCRIPT(error == (scenario == Scenario::failed_submit ? EIO : ECANCELED));
                script.EXPECT_SCRIPT(ctrl.added_dev_id() == -1);
            } else {
                script.EXPECT_SCRIPT(error == 0 && id == 73);
                script.EXPECT_SCRIPT(ctrl.added_dev_id() == 73);
            }
            script.EXPECT_SCRIPT(script.iteration == (scenario == Scenario::short_submit ? 5 : 4));
        }
    } // Normal settled commands must also permit Ctrl destruction.
    ::_exit(0);
}
#undef EXPECT_SCRIPT

struct ProbeResult {
    int status = -1;
    int failure_line = 0;
    bool timed_out = false;
};

ProbeResult probe(Scenario scenario) {
    int diagnostic[2];
    if (::pipe(diagnostic) < 0) return {};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(diagnostic[0]);
        exercise(scenario, diagnostic[1]);
        ::_exit(101);
    }
    ::close(diagnostic[1]);
    if (pid < 0) {
        ::close(diagnostic[0]);
        return {};
    }
    ProbeResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const pid_t reaped = ::waitpid(pid, &result.status, WNOHANG);
        if (reaped == pid) break;
        if (reaped < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            // This exact unreaped child is ours; kill before reap, never after.
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &result.status, 0) < 0 && errno == EINTR) {}
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (::read(diagnostic[0], &result.failure_line, sizeof(result.failure_line)) < 0 &&
           errno == EINTR) {}
    ::close(diagnostic[0]);
    return result;
}

void require_probe(Scenario scenario) {
    const auto result = probe(scenario);
    INFO("script failure line: " << result.failure_line);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.status >= 0);
    REQUIRE(WIFEXITED(result.status));
    REQUIRE(WEXITSTATUS(result.status) == 0);
    REQUIRE(result.failure_line == 0);
}
} // namespace

TEST_CASE("ublk: actual control loop waits for original and cancellation CQEs", "[ublk][ctrl]") {
    SECTION("cancellation completes before successful ADD") { require_probe(Scenario::cancel_first); }
    SECTION("successful ADD commits ownership before late cancellation") { require_probe(Scenario::original_first); }
    SECTION("cancel ENOENT cannot retire a pending original") { require_probe(Scenario::cancel_not_found); }
}

TEST_CASE("ublk: actual control loop retains payload across ambiguous and short submissions", "[ublk][ctrl]") {
    SECTION("submit error after publication") { require_probe(Scenario::failed_submit); }
    SECTION("zero submit followed by partial submit") { require_probe(Scenario::short_submit); }
    SECTION("no SQE means no published command") { require_probe(Scenario::no_sqe); }
}

TEST_CASE("ublk: actual control loop retains poisoned completion ownership", "[ublk][ctrl]") {
    SECTION("unknown cookie") { require_probe(Scenario::unknown_cqe); }
    SECTION("duplicate original CQE") { require_probe(Scenario::duplicate_cqe); }
    SECTION("unexpected multishot CQE") { require_probe(Scenario::multishot_cqe); }
}
