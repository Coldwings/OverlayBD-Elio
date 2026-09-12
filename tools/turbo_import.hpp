#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace obd::convert {
// Offline converter helper: verifies package/target hashes against a TurboOCI
// descriptor, validates native metadata, and publishes into a new directory.
// Must be called outside an active Elio scheduler.
nlohmann::json import_turbo_image(const std::string& package_path,
                                  const std::string& descriptor_path,
                                  const std::string& target_path,
                                  const std::string& destination_dir);
}
