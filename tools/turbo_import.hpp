#pragma once
#include <nlohmann/json.hpp>
#include <string>
#ifdef OBD_TEST_TURBO_IMPORT_HOOK
#include <functional>
#endif

namespace obd::convert {
// Offline converter helper: verifies package/target hashes against a TurboOCI
// descriptor, validates native metadata, and publishes into a new directory.
// Differential layers require a local parent config; its complete UUID chain
// is validated and retained in the returned lower stack.
// Must be called outside an active Elio scheduler.
nlohmann::json import_turbo_image(const std::string& package_path,
                                  const std::string& descriptor_path,
                                  const std::string& target_path,
                                  const std::string& destination_dir,
                                  const std::string& parent_config_path = {}
#ifdef OBD_TEST_TURBO_IMPORT_HOOK
                                  , const std::function<void()>& before_publish = {}
#endif
                                  );
}
