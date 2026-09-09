// obdctl: control CLI for obd-supervisor (docs/supervisor.md). One
// JSON-lines command per invocation over the supervisor UDS; prints the
// reply. Plain blocking IO — no Elio runtime needed.
#include "supervisor/protocol.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char* kDefaultSocket = "/run/overlaybd-elio/supervisor.sock";

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s [--socket PATH] hello\n"
                 "  %s [--socket PATH] create <id> <config.json> [--global PATH] [--dev-id N] [--virtual-size BYTES]\n"
                 "  %s [--socket PATH] create-blank <id> --size BYTES [--mkfs TYPE] [--global PATH] [--dev-id N]\n"
                 "  %s [--socket PATH] destroy <id>\n"
                 "  %s [--socket PATH] list\n"
                 "  %s [--socket PATH] status <id>\n"
                 "  %s [--socket PATH] commit <id> [--tag TAG] [--virtual-size BYTES]\n"
                 "  %s [--socket PATH] trace_start <id> <output.trace> [--duration SEC]\n"
                 "  %s [--socket PATH] trace_stop <id>\n"
                 "  %s [--socket PATH] resize <id> <size-bytes>\n",
                 argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
                 argv0, argv0);
}

bool send_all(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w <= 0) return false;
        done += static_cast<size_t>(w);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = kDefaultSocket;
    int i = 1;
    while (i < argc && std::string(argv[i]) == "--socket") {
        if (++i >= argc) {
            usage(argv[0]);
            return 2;
        }
        socket_path = argv[i++];
    }
    if (i >= argc) {
        usage(argv[0]);
        return 2;
    }
    const std::string cmd = argv[i++];

    nlohmann::json req;
    req["cmd"] = cmd;
    if (cmd == "create") {
        if (i + 2 > argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
        req["config"] = argv[i++];
        while (i < argc) {
            const std::string a = argv[i++];
            if (a == "--global" && i < argc) req["global"] = argv[i++];
            else if (a == "--dev-id" && i < argc)
                req["dev_id"] = std::stoi(argv[i++]);
            else if (a == "--virtual-size" && i < argc) {
                // D3 headroom override (bytes): full strtoull validation
                // for a fast, clear error; the grow-only/alignment
                // semantics are decided where sizes are comparable
                // (supervisor + device).
                const char* v = argv[i++];
                if (v[0] == '-') {
                    std::fprintf(stderr,
                                 "invalid --virtual-size '%s' (want a "
                                 "positive byte count)\n",
                                 v);
                    return 2;
                }
                char* end = nullptr;
                errno = 0;
                const unsigned long long b = std::strtoull(v, &end, 10);
                if (errno != 0 || end == v || *end != '\0' || b == 0) {
                    std::fprintf(stderr,
                                 "invalid --virtual-size '%s' (want a "
                                 "positive byte count)\n",
                                 v);
                    return 2;
                }
                req["virtual_size"] = b;
            } else {
                usage(argv[0]);
                return 2;
            }
        }
    } else if (cmd == "create-blank") {
        // ADR-0014 modes 2/3: build the wire create with the additive
        // "blank" object (size mandatory, mkfs optional).
        if (i >= argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
        nlohmann::json blank;
        bool have_size = false;
        while (i < argc) {
            const std::string a = argv[i++];
            if (a == "--size" && i < argc) {
                const char* v = argv[i++];
                char* end = nullptr;
                errno = 0;
                const long long n = std::strtoll(v, &end, 10);
                if (errno != 0 || end == v || *end != '\0' || n <= 0 ||
                    n % 512 != 0) {
                    std::fprintf(stderr,
                                 "invalid --size '%s' (want a positive "
                                 "multiple of 512 bytes)\n",
                                 v);
                    return 2;
                }
                blank["size"] = static_cast<uint64_t>(n);
                have_size = true;
            } else if (a == "--mkfs" && i < argc) {
                const char* v = argv[i++];
                // Local charset mirror of valid_mkfs_type (supervisor-side
                // validation is authoritative).
                const std::string t(v);
                if (t.empty() || t.size() > 16 ||
                    !std::all_of(t.begin(), t.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '_';
                    })) {
                    std::fprintf(stderr,
                                 "invalid --mkfs '%s' (want a 1..16 char "
                                 "lowercase alphanumeric type)\n",
                                 v);
                    return 2;
                }
                blank["mkfs"] = t;
            } else if (a == "--global" && i < argc) req["global"] = argv[i++];
            else if (a == "--dev-id" && i < argc)
                req["dev_id"] = std::stoi(argv[i++]);
            else {
                usage(argv[0]);
                return 2;
            }
        }
        if (!have_size) {
            std::fprintf(stderr, "create-blank requires --size BYTES\n");
            return 2;
        }
        req["blank"] = std::move(blank);
    } else if (cmd == "destroy" || cmd == "status") {
        if (i >= argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
    } else if (cmd == "commit") {
        if (i >= argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
        while (i < argc) {
            const std::string a = argv[i++];
            if (a == "--tag" && i < argc) req["user_tag"] = argv[i++];
            else if (a == "--virtual-size" && i < argc) {
                // D3 commit re-baseline override (bytes): full strtoull
                // validation; grow-only/alignment semantics are decided
                // where the upper is readable (supervisor + seal path).
                const char* v = argv[i++];
                if (v[0] == '-') {
                    std::fprintf(stderr,
                                 "invalid --virtual-size '%s' (want a "
                                 "positive byte count)\n",
                                 v);
                    return 2;
                }
                char* end = nullptr;
                errno = 0;
                const unsigned long long b = std::strtoull(v, &end, 10);
                if (errno != 0 || end == v || *end != '\0' || b == 0) {
                    std::fprintf(stderr,
                                 "invalid --virtual-size '%s' (want a "
                                 "positive byte count)\n",
                                 v);
                    return 2;
                }
                req["virtual_size"] = b;
            } else {
                usage(argv[0]);
                return 2;
            }
        }
    } else if (cmd == "trace_start") {
        if (i + 2 > argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
        req["path"] = argv[i++];
        req["duration_sec"] = 300;  // runbook default (docs/operations.md)
        while (i < argc) {
            const std::string a = argv[i++];
            if (a == "--duration" && i < argc) {
                // std::stoi would abort the CLI on bad input; validate
                // fully, mirroring the device-side bound [1, 3600]
                // (TraceRecorder::kMin/MaxDurationSec) for a fast,
                // clear error instead of a device rejection.
                const char* v = argv[i++];
                char* end = nullptr;
                errno = 0;
                const long dur = std::strtol(v, &end, 10);
                if (errno != 0 || end == v || *end != '\0' || dur < 1 ||
                    dur > 3600) {
                    std::fprintf(stderr,
                                 "invalid --duration '%s' "
                                 "(want an integer in 1..3600 seconds)\n",
                                 v);
                    return 2;
                }
                req["duration_sec"] = dur;
            } else {
                usage(argv[0]);
                return 2;
            }
        }
    } else if (cmd == "trace_stop") {
        if (i >= argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
        if (i != argc) {
            // Silent extra-arg acceptance would mask typos (trace_start
            // validates its full remainder too).
            usage(argv[0]);
            return 2;
        }
    } else if (cmd == "resize") {
        if (i + 2 > argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
        // Byte count: full strtoull validation for a fast, clear error
        // instead of a supervisor/device rejection (grow-only itself is
        // decided device-side, where the current size is known). A
        // negative value wraps through unsigned; reject it explicitly.
        const char* v = argv[i++];
        if (v[0] == '-') {
            std::fprintf(stderr,
                         "invalid resize size '%s' (want a positive byte "
                         "count)\n",
                         v);
            return 2;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long long bytes = std::strtoull(v, &end, 10);
        if (errno != 0 || end == v || *end != '\0' || bytes == 0) {
            std::fprintf(stderr,
                         "invalid resize size '%s' (want a positive byte "
                         "count)\n",
                         v);
            return 2;
        }
        req["size"] = bytes;
        if (i != argc) {
            usage(argv[0]);
            return 2;
        }
    } else if (cmd == "list" || cmd == "hello") {
        // no fields
    } else {
        usage(argv[0]);
        return 2;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    sockaddr_un sa {};
    sa.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(sa.sun_path)) {
        std::fprintf(stderr, "socket path too long\n");
        return 2;
    }
    std::strcpy(sa.sun_path, socket_path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        std::fprintf(stderr, "cannot connect to %s: %s\n",
                     socket_path.c_str(), std::strerror(errno));
        return 1;
    }
    std::string line = req.dump() + "\n";
    if (!send_all(fd, line)) {
        std::perror("write");
        return 1;
    }
    std::string reply;
    char buf[4096];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        reply.append(buf, static_cast<size_t>(r));
        if (reply.find('\n') != std::string::npos) break;
    }
    ::close(fd);
    if (reply.empty()) {
        std::fprintf(stderr, "no reply from supervisor\n");
        return 1;
    }
    try {
        const auto j = nlohmann::json::parse(reply);
        std::puts(j.dump(2).c_str());
        return j.value("ok", false) ? 0 : 1;
    } catch (const nlohmann::json::exception&) {
        std::fputs(reply.c_str(), stdout);
        return 1;
    }
}
