// obdctl: control CLI for obd-supervisor (docs/supervisor.md). One
// JSON-lines command per invocation over the supervisor UDS; prints the
// reply. Plain blocking IO — no Elio runtime needed.
#include "supervisor/protocol.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* kDefaultSocket = "/run/overlaybd-elio/supervisor.sock";

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s [--socket PATH] hello\n"
                 "  %s [--socket PATH] create <id> <config.json> [--global PATH] [--dev-id N]\n"
                 "  %s [--socket PATH] destroy <id>\n"
                 "  %s [--socket PATH] list\n"
                 "  %s [--socket PATH] status <id>\n",
                 argv0, argv0, argv0, argv0, argv0);
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
            else {
                usage(argv[0]);
                return 2;
            }
        }
    } else if (cmd == "destroy" || cmd == "status") {
        if (i >= argc) {
            usage(argv[0]);
            return 2;
        }
        req["id"] = argv[i++];
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
