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
    if (cmd == "create") {
        if (!j.contains("id") || !j.contains("config")) {
            error = "create requires 'id' and 'config'";
            return std::nullopt;
        }
    } else if (cmd == "destroy" || cmd == "status") {
        if (!j.contains("id")) {
            error = cmd + " requires 'id'";
            return std::nullopt;
        }
    } else if (cmd == "commit") {
        if (!j.contains("id")) {
            error = "commit requires 'id'";
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
    // offline commit command is served.
    fields["features"] = nlohmann::json::array({"commit"});
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
