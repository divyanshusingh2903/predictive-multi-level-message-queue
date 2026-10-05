#pragma once

#include "harbinger_service.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace harbinger {

inline constexpr std::size_t kMaxConfigFileBytes = 1024 * 1024;

/// Standalone-server settings that live outside HarbingerConfig.
struct ServerSettings {
    std::string listen_address{"0.0.0.0:50051"};
    /// Period of the JSON stats line on stdout; zero disables it.
    std::chrono::milliseconds stats_interval{0};
};

/// Apply a version-1 configuration document on top of config/server (defaults first, then the file).
/// Every field is optional; unknown fields, wrong types, out-of-range values and duplicate keys are rejected.
/// The result is validated again by HarbingerService's own constructor.
void apply_config_document(std::string_view document, HarbingerConfig& config, ServerSettings& server);

/// Read a bounded regular file and apply it with apply_config_document.
void apply_config_file(const std::filesystem::path& path, HarbingerConfig& config, ServerSettings& server);

/// One-line human-readable summary of the effective configuration; prints no header values or payload data.
[[nodiscard]] std::string describe_config(const HarbingerConfig& config, const ServerSettings& server);

/// Single-line JSON with queue, feedback and routing counters for the stats reporter.
[[nodiscard]] std::string stats_json(const HarbingerService& service);

} // namespace harbinger
