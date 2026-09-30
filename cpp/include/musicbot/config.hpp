#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace musicbot {

struct Config {
    std::string token;
    std::optional<std::uint64_t> guild_id;
    int default_volume_percent{50};
    std::int64_t max_track_duration{10800};
    int idle_timeout_seconds{300};
    int buffer_ahead_seconds{60};
    int prebuffer_seconds{2};
    std::filesystem::path yt_dlp_path{"yt-dlp"};
    std::filesystem::path deno_path{"deno"};
    std::optional<std::filesystem::path> youtube_cookies_file;
    std::optional<std::filesystem::path> configuration_path;

    static Config load(const std::filesystem::path& executable_path);
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace musicbot
