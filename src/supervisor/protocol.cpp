// Supervisor wire protocol. See protocol.hpp.
#include "supervisor/protocol.hpp"

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
        if (!j.contains("id") || !j.contains("config")) {
            error = "create requires 'id' and 'config'";
            return std::nullopt;
        }
        if (!j["id"].is_string() || !j["config"].is_string()) {
            error = "create 'id' and 'config' must be strings";
            return std::nullopt;
        }
        if ((j.contains("global") && !j["global"].is_string()) ||
            (j.contains("device_bin") && !j["device_bin"].is_string()) ||
            (j.contains("dev_id") && !j["dev_id"].is_number_integer())) {
            error = "create 'global'/'device_bin' must be strings, "
                    "'dev_id' an integer";
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
    } else if (cmd != "list" && cmd != "hello") {
        error = "unknown cmd '" + cmd + "'";
        return std::nullopt;
    }
    return j;
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
    // path (trace_start/trace_stop) is served.
    fields["features"] = nlohmann::json::array({"commit", "trace"});
    return reply_ok(fields);
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
    DeviceStatus st;
    st.state = j["state"].get<std::string>();
    st.device = j.value("device", "");
    st.error = j.value("error", "");
    return st;
}

std::string make_device_status(const DeviceStatus& st) {
    nlohmann::json j;
    j["state"] = st.state;
    if (!st.device.empty()) j["device"] = st.device;
    if (!st.error.empty()) j["error"] = st.error;
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
