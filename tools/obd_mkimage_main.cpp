// obd-mkimage: builds OverlayBD-format test images (docs/testing.md):
// a raw disk image becomes a sealed single-layer LSMT, optionally
// ZFile-compressed, and a lowers[] config snippet is printed. Synchronous
// cold path (format/writer.hpp).
#include "common/errors.hpp"
#include "common/sha256.hpp"
#include "format/writer.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --input <raw.img> --out-dir <dir> [--name base]\n"
                 "          [--zfile] [--bs N] [--zstd [level]] [--no-verify]\n"
                 "\n"
                 "Produces <out-dir>/<name>.lsmt and, with --zfile, the\n"
                 "compressed blob <out-dir>/<name>.zfile plus a lowers[]\n"
                 "config snippet (digest/size/file) on stdout.\n",
                 argv0);
}

std::string sha256_of_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) obd::throw_errno(errno, "cannot open " + path);
    struct Guard {
        int fd;
        ~Guard() { ::close(fd); }
    } g{fd};
    obd::common::Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r < 0) obd::throw_errno(errno, "read failed on " + path);
        if (r == 0) break;
        h.update(buf.data(), static_cast<size_t>(r));
    }
    return h.final_hex();
}

uint64_t file_size(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        obd::throw_errno(errno, "cannot stat " + path);
    }
    return static_cast<uint64_t>(st.st_size);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input, out_dir, name = "layer";
    bool zfile = false;
    bool verify = true;
    obd::format::ZFileWriteOptions zopts;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (++i >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[i];
        };
        if (a == "--input") input = next("--input");
        else if (a == "--out-dir") out_dir = next("--out-dir");
        else if (a == "--name") name = next("--name");
        else if (a == "--zfile") zfile = true;
        else if (a == "--bs") zopts.block_size = std::stoul(next("--bs"));
        else if (a == "--zstd") {
            zopts.algo = obd::format::zfile::kAlgoZstd;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                zopts.level = static_cast<uint8_t>(std::stoi(argv[++i]));
            }
        } else if (a == "--no-verify") {
            verify = false;
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (input.empty() || out_dir.empty()) {
        usage(argv[0]);
        return 2;
    }
    zopts.verify = verify;

    try {
        const int in_fd = ::open(input.c_str(), O_RDONLY | O_CLOEXEC);
        if (in_fd < 0) obd::throw_errno(errno, "cannot open " + input);
        struct Guard {
            int fd;
            ~Guard() { ::close(fd); }
        } g{in_fd};
        const uint64_t in_size = file_size(input);

        const std::string lsmt_path = out_dir + "/" + name + ".lsmt";
        obd::format::write_lsmt_single_layer(in_fd, in_size, lsmt_path);
        std::string blob = lsmt_path;

        if (zfile) {
            const std::string zfile_path = out_dir + "/" + name + ".zfile";
            const int lsmt_fd =
                ::open(lsmt_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (lsmt_fd < 0) obd::throw_errno(errno, "cannot open " + lsmt_path);
            struct Guard2 {
                int fd;
                ~Guard2() { ::close(fd); }
            } g2{lsmt_fd};
            obd::format::write_zfile(lsmt_fd, file_size(lsmt_path),
                                     zfile_path, zopts);
            blob = zfile_path;
        }

        const std::string digest = "sha256:" + sha256_of_file(blob);
        nlohmann::json lower;
        lower["digest"] = digest;
        lower["size"] = file_size(blob);
        lower["file"] = blob;
        nlohmann::json snippet;
        snippet["repoBlobUrl"] = "";
        snippet["lowers"] = nlohmann::json::array({lower});
        std::puts(snippet.dump(2).c_str());
        return 0;
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "obd-mkimage: %s\n", e.what());
        return 1;
    }
}
