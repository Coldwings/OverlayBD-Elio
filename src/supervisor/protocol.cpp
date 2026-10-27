// Supervisor wire protocol. See protocol.hpp.
#include "supervisor/protocol.hpp"

#include <climits>
#include <cstdint>

namespace obd::supervisor {

std::optional<nlohmann::json> parse_command(std::string_view line,
                                            std::string& error) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("malformed JSON: ") + e.what();
        return std::nullopt;
    }
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) {
        error = "command object with a string 'cmd' field required";
        return std::nullopt;
    }
    const std::string cmd = j["cmd"].get<std::string>();
    // Field types are validated here — not in the handlers — so a
    // malformed field is answered with a clean protocol error instead of
    // a json::type_error escaping the handler.
    if (cmd == "create") {
        if (!j.contains("id")) {
            error = "create requires 'id'";
            return std::nullopt;
        }
        if (!j["id"].is_string()) {
            error = "create 'id' must be a string";
            return std::nullopt;
        }
        // ADR-0014 three creation modes, one create command: from an image
        // (`config`, mandatory for that mode) or blank raw (`blank`, size +
        // optional mkfs). Exactly one of the two must be present — old
        // supervisors ignore the additive `blank` object and would answer
        // the config-less form with their own "requires config" error.
        const bool has_config = j.contains("config");
        const bool has_blank = j.contains("blank");
        if (has_config == has_blank) {
            error = "create requires exactly one of 'config' (image) or "
                    "'blank' (raw disk)";
            return std::nullopt;
        }
        if (has_config) {
            if (!j["config"].is_string()) {
                error = "create 'config' must be a string";
                return std::nullopt;
            }
        } else {  // has_blank
            if (!j["blank"].is_object()) {
                error = "create 'blank' must be an object";
                return std::nullopt;
            }
            if (!j["blank"].contains("size")) {
                error = "create blank requires 'size'";
                return std::nullopt;
            }
            const auto& size = j["blank"]["size"];
            const bool ok_int =
                size.is_number_unsigned() ||
                (size.is_number_integer() && size.get<int64_t>() >= 0);
            if (!ok_int) {
                error = "create blank 'size' must be a non-negative integer";
                return std::nullopt;
            }
            if (j["blank"].contains("mkfs") &&
                !j["blank"]["mkfs"].is_string()) {
                error = "create blank 'mkfs' must be a string";
                return std::nullopt;
            }
        }
        if ((j.contains("global") && !j["global"].is_string()) ||
            (j.contains("device_bin") && !j["device_bin"].is_string()) ||
            (j.contains("dev_id") && !j["dev_id"].is_number_integer())) {
            error = "create 'global'/'device_bin' must be strings, "
                    "'dev_id' an integer";
            return std::nullopt;
        }
        // 'dev_id' is an OPTIONAL kernel device id (-1/absent = auto).
        // Validate in the JSON integer's original domain before narrowing:
        // UINT64_MAX would otherwise become -1 and request automatic
        // allocation instead of reporting an invalid explicit device ID.
        if (j.contains("dev_id")) {
            const auto& value = j["dev_id"];
            const bool in_range = value.is_number_unsigned()
                ? value.get<uint64_t>() <= uint64_t{INT32_MAX}
                : value.get<int64_t>() >= -1 &&
                  value.get<int64_t>() <= INT32_MAX;
            if (!in_range) {
                error = "create 'dev_id' must be an integer in [-1, INT32_MAX]";
                return std::nullopt;
            }
        }
        // D3 create-time headroom: optional 'virtual_size' override in
        // bytes. Semantic rules (positive, 512-aligned, grow-only vs the
        // image size) live in the handlers, where the image's declared
        // size is known (the device's opened.virtual_size).
        if (j.contains("virtual_size") &&
            !j["virtual_size"].is_number_unsigned() &&
            !(j["virtual_size"].is_number_integer() &&
              j["virtual_size"].get<int64_t>() >= 0)) {
            error = "create 'virtual_size' must be a non-negative integer";
            return std::nullopt;
        }
    } else if (cmd == "destroy" || cmd == "status") {
        if (!j.contains("id")) {
            error = cmd + " requires 'id'";
            return std::nullopt;
        }
        if (!j["id"].is_string()) {
            error = cmd + " 'id' must be a string";
            return std::nullopt;
        }
    } else if (cmd == "commit") {
        if (!j.contains("id")) {
            error = "commit requires 'id'";
            return std::nullopt;
        }
        if (!j["id"].is_string()) {
            error = "commit 'id' must be a string";
            return std::nullopt;
        }
        if (j.contains("user_tag") && !j["user_tag"].is_string()) {
            error = "commit 'user_tag' must be a string";
            return std::nullopt;
        }
        // D3 commit re-baseline: optional 'virtual_size' override in
        // bytes (0 = absent). Semantic rules (positive, 512-aligned,
        // grow-only vs the layer's declared size and content extent)
        // live in the handlers/seal path, where the upper is readable.
        if (j.contains("virtual_size") &&
            !j["virtual_size"].is_number_unsigned() &&
            !(j["virtual_size"].is_number_integer() &&
              j["virtual_size"].get<int64_t>() >= 0)) {
            error = "commit 'virtual_size' must be a non-negative integer";
            return std::nullopt;
        }
    } else if (cmd == "trace_start") {
        if (!j.contains("id") || !j.contains("path") ||
            !j.contains("duration_sec")) {
            error = "trace_start requires 'id', 'path' and 'duration_sec'";
            return std::nullopt;
        }
        if (!j["id"].is_string() || !j["path"].is_string()) {
            error = "trace_start 'id' and 'path' must be strings";
            return std::nullopt;
        }
        if (!j["duration_sec"].is_number_integer()) {
            error = "trace_start 'duration_sec' must be an integer";
            return std::nullopt;
        }
    } else if (cmd == "trace_stop") {
        if (!j.contains("id")) {
            error = "trace_stop requires 'id'";
            return std::nullopt;
        }
        if (!j["id"].is_string()) {
            error = "trace_stop 'id' must be a string";
            return std::nullopt;
        }
    } else if (cmd == "resize") {
        if (!j.contains("id") || !j.contains("size")) {
            error = "resize requires 'id' and 'size'";
            return std::nullopt;
        }
        if (!j["id"].is_string()) {
            error = "resize 'id' must be a string";
            return std::nullopt;
        }
        // 'size' is a byte count: accept unsigned or non-negative signed
        // integers (semantic rules — positive, 512-aligned, grow-only —
        // live in the handlers, which know the current size).
        if (!j["size"].is_number_unsigned() &&
            !(j["size"].is_number_integer() &&
              j["size"].get<int64_t>() >= 0)) {
            error = "resize 'size' must be a non-negative integer (bytes)";
            return std::nullopt;
        }
    } else if (cmd != "list" && cmd != "hello") {
        error = "unknown cmd '" + cmd + "'";
        return std::nullopt;
    }
    return j;
}

bool valid_mkfs_type(const std::string& type) {
    if (type.empty() || type.size() > 16) return false;
    for (const char c : type) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

std::optional<BlankSpec> parse_blank_spec(const nlohmann::json& blank,
                                          std::string& error) {
    if (!blank.is_object() || !blank.contains("size")) {
        error = "'blank' object with 'size' required";
        return std::nullopt;
    }
    const auto& size = blank["size"];
    uint64_t vsize = 0;
    if (size.is_number_unsigned()) {
        vsize = size.get<uint64_t>();
    } else if (size.is_number_integer() && size.get<int64_t>() >= 0) {
        vsize = static_cast<uint64_t>(size.get<int64_t>());
    } else {
        error = "blank 'size' must be a non-negative integer";
        return std::nullopt;
    }
    if (vsize == 0) {
        error = "blank 'size' must be positive";
        return std::nullopt;
    }
    if (vsize % 512 != 0) {
        error = "blank 'size' must be a multiple of 512";
        return std::nullopt;
    }
    if (vsize > kMaxBlankSizeBytes) {
        error = "blank 'size' exceeds the sanity bound of " +
                std::to_string(kMaxBlankSizeBytes) + " bytes";
        return std::nullopt;
    }
    BlankSpec spec;
    spec.size = vsize;
    if (blank.contains("upper_type")) {
        if (!blank["upper_type"].is_string()) {
            error = "blank 'upper_type' must be a string";
            return std::nullopt;
        }
        spec.upper_type = blank["upper_type"].get<std::string>();
        if (spec.upper_type != "lsmt" && spec.upper_type != "hybrid") {
            error = "blank 'upper_type' must be lsmt or hybrid";
            return std::nullopt;
        }
    }
    if (blank.contains("mkfs")) {
        const std::string mkfs = blank["mkfs"].get<std::string>();
        if (!valid_mkfs_type(mkfs)) {
            error = "invalid blank 'mkfs' type '" + mkfs +
                    "' (want mkfs.<type> with a 1..16 char [a-z0-9_] "
                    "type)";
            return std::nullopt;
        }
        spec.mkfs = mkfs;
    }
    return spec;
}

std::string reply_ok(const nlohmann::json& fields) {
    nlohmann::json j = fields.is_object() ? fields : nlohmann::json::object();
    j["ok"] = true;
    return j.dump() + "\n";
}

std::string reply_error(const std::string& error) {
    nlohmann::json j;
    j["ok"] = false;
    j["error"] = error;
    return j.dump() + "\n";
}

std::string reply_hello() {
    nlohmann::json fields;
    fields["protocol"] = kProtocolVersion;
    fields["version"] = kProjectVersion;
    // Capability gate (additive-only rule): "commit" = the ADR-0014
    // offline commit command is served; "trace" = the ADR-0013 record
    // path (trace_start/trace_stop) is served; "resize" = the D3
    // grow-only online resize command is served; "blank" = create
    // accepts the ADR-0014 blank (raw) device modes 2/3.
    fields["features"] =
        nlohmann::json::array({"commit", "trace", "resize", "blank", "startup_owner", "hybrid_upper"});
    return reply_ok(fields);
}

namespace {

bool owner_phase(std::string_view phase) {
    return phase == "preparing" || phase == "control_pending" ||
           phase == "retained" || phase == "releasing" ||
           phase == "ready" || phase == "released";
}

bool owner_release(std::string_view release) {
    return release == "none" || release == "pending" ||
           release == "refused" || release == "released";
}

bool nonnegative_integer_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return false;
    const auto& value = j[key];
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<int64_t>() >= 0);
}

bool int32_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return false;
    const auto& value = j[key];
    // Compare in the stored integer domain before narrowing. JSON's mixed
    // signed/unsigned comparisons can convert UINT64_MAX to a negative value.
    if (value.is_number_unsigned())
        return value.get<uint64_t>() <= uint64_t{INT32_MAX};
    if (!value.is_number_integer()) return false;
    const auto number = value.get<int64_t>();
    return number >= INT32_MIN && number <= INT32_MAX;
}

} // namespace

nlohmann::json startup_owner_json(const StartupOwnerStatus& owner) {
    return {{"version", kStartupOwnerVersion},
            {"generation", owner.generation}, {"revision", owner.revision},
            {"phase", owner.phase}, {"device_id", owner.device_id},
            {"reason_errno", owner.reason_errno},
            {"control_pending", owner.control_pending},
            {"kernel_debt_known", owner.kernel_debt_known},
            {"raw_published", owner.raw_published},
            {"raw_retired", owner.raw_retired},
            {"release_sequence", owner.release_sequence},
            {"release", owner.release}, {"release_errno", owner.release_errno}};
}

bool valid_startup_owner(const StartupOwnerStatus& owner) noexcept {
    if (owner.generation == 0 || !owner_phase(owner.phase) ||
        !owner_release(owner.release) || owner.device_id < -1 ||
        owner.reason_errno < 0 || owner.release_errno < 0 ||
        owner.release_sequence >= (uint64_t{1} << 63) ||
        owner.raw_retired > owner.raw_published) return false;
    if (owner.release == "none") {
        if (owner.release_sequence != 0 || owner.release_errno != 0) return false;
    } else {
        if (owner.release_sequence == 0) return false;
        if (owner.release == "refused") {
            if (owner.release_errno == 0 || owner.phase != "retained") return false;
        } else if (owner.release_errno != 0) return false;
    }
    if (owner.phase == "released" &&
        (owner.control_pending || !owner.kernel_debt_known ||
         owner.raw_retired != owner.raw_published ||
         (owner.release != "none" && owner.release != "released"))) return false;
    if (owner.release == "released" && owner.phase != "released") return false;
    if (owner.phase == "ready" &&
        (owner.control_pending || !owner.kernel_debt_known ||
         owner.reason_errno != 0 || owner.release != "none")) return false;
    return true;
}

std::optional<StartupOwnerStatus> parse_startup_owner(const nlohmann::json& j) {
    try {
        if (!j.is_object() || !j.contains("version") ||
            !j["version"].is_number_integer() ||
            j["version"] != kStartupOwnerVersion ||
            !nonnegative_integer_field(j, "generation") ||
            j["generation"].get<uint64_t>() == 0 ||
            !nonnegative_integer_field(j, "revision") ||
            !nonnegative_integer_field(j, "raw_published") ||
            !nonnegative_integer_field(j, "raw_retired") ||
            !nonnegative_integer_field(j, "release_sequence"))
            return std::nullopt;
        for (const char* key : {"device_id", "reason_errno", "release_errno"}) {
            if (!int32_field(j, key)) return std::nullopt;
        }
        StartupOwnerStatus owner;
        owner.generation = j["generation"].get<uint64_t>();
        owner.revision = j["revision"].get<uint64_t>();
        owner.phase = j.at("phase").get<std::string>();
        owner.device_id = j["device_id"].get<int>();
        owner.reason_errno = j["reason_errno"].get<int>();
        owner.control_pending = j.at("control_pending").get<bool>();
        owner.kernel_debt_known = j.at("kernel_debt_known").get<bool>();
        owner.raw_published = j["raw_published"].get<uint64_t>();
        owner.raw_retired = j["raw_retired"].get<uint64_t>();
        owner.release_sequence = j["release_sequence"].get<uint64_t>();
        owner.release = j.at("release").get<std::string>();
        owner.release_errno = j["release_errno"].get<int>();
        if (!valid_startup_owner(owner)) return std::nullopt;
        return owner;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<DeviceStatus> parse_device_status(std::string_view line) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!j.is_object() || !j.contains("state") || !j["state"].is_string()) {
        return std::nullopt;
    }
    try {
        DeviceStatus st;
        st.state = j["state"].get<std::string>();
        st.device = j.value("device", "");
        st.error = j.value("error", "");
        if (j.contains("startup_owner")) {
            st.startup_owner = parse_startup_owner(j["startup_owner"]);
            if (!st.startup_owner) return std::nullopt;
        }
        return st;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::string make_device_status(const DeviceStatus& st) {
    nlohmann::json j;
    j["state"] = st.state;
    if (!st.device.empty()) j["device"] = st.device;
    if (!st.error.empty()) j["error"] = st.error;
    if (st.startup_owner) j["startup_owner"] = startup_owner_json(*st.startup_owner);
    return j.dump() + "\n";
}

bool is_device_reply_line(const nlohmann::json& j) {
    return j.is_object() && j.contains("reply") && j["reply"].is_string();
}

int dev_id_from_bdev_path(const std::string& path) {
    const std::string prefix = "/dev/ublkb";
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return -1;
    }
    int id = 0;
    for (size_t i = prefix.size(); i < path.size(); ++i) {
        if (path[i] < '0' || path[i] > '9') return -1;
        id = id * 10 + (path[i] - '0');
    }
    return id;
}

}  // namespace obd::supervisor
