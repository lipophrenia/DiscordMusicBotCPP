#include "config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace musicbot {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

using EnvironmentFile = std::unordered_map<std::string, std::string>;

bool load_env_file(const std::filesystem::path& path, EnvironmentFile& values) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.starts_with("export ")) {
            line.erase(0, 7);
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        auto name = trim(line.substr(0, separator));
        auto value = trim(line.substr(separator + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (!name.empty()) {
            values.try_emplace(std::move(name), std::move(value));
        }
    }
    return true;
}

std::string system_environment(const char* name, std::string fallback = {}) {
    if (const auto* value = std::getenv(name)) {
        return trim(value);
    }
    return fallback;
}

std::string config_value(
    const EnvironmentFile& values,
    const char* name,
    std::string fallback = {}) {
    if (const auto* value = std::getenv(name)) {
        return trim(value);
    }
    if (const auto iterator = values.find(name); iterator != values.end()) {
        return iterator->second;
    }
    return fallback;
}

template <typename Integer>
Integer parse_integer(
    const EnvironmentFile& values,
    const char* name,
    Integer fallback,
    Integer minimum,
    std::optional<Integer> maximum = std::nullopt) {
    const auto raw = config_value(values, name);
    if (raw.empty()) {
        return fallback;
    }
    Integer value{};
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (error != std::errc{} || end != raw.data() + raw.size()) {
        throw ConfigError(std::string(name) + " must be an integer");
    }
    if (value < minimum || (maximum && value > *maximum)) {
        throw ConfigError(std::string(name) + " is outside the allowed range");
    }
    return value;
}

std::filesystem::path user_config_path() {
#ifdef _WIN32
    const auto base = system_environment("APPDATA");
    return base.empty() ? std::filesystem::path{} :
                          std::filesystem::path(base) / "DiscordMusicBot" / ".env";
#elif defined(__APPLE__)
    const auto home = system_environment("HOME");
    return home.empty() ? std::filesystem::path{} :
                          std::filesystem::path(home) / "Library" / "Application Support" /
                              "DiscordMusicBot" / ".env";
#else
    auto base = system_environment("XDG_CONFIG_HOME");
    if (base.empty()) {
        const auto home = system_environment("HOME");
        if (!home.empty()) {
            base = (std::filesystem::path(home) / ".config").string();
        }
    }
    return base.empty() ? std::filesystem::path{} :
                          std::filesystem::path(base) / "DiscordMusicBot" / ".env";
#endif
}

std::filesystem::path running_executable(const std::filesystem::path& argument) {
#if defined(__linux__)
    std::error_code error;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !resolved.empty()) {
        return resolved;
    }
#endif
    if (!argument.has_parent_path()) {
        return argument;
    }
    std::error_code fallback_error;
    auto fallback = std::filesystem::absolute(argument, fallback_error);
    return fallback_error ? argument : fallback;
}

std::filesystem::path default_helper_path(
    const std::filesystem::path& executable,
    const std::filesystem::path& filename) {
    std::vector<std::filesystem::path> candidates;
    const auto resolved_executable = running_executable(executable);
    if (resolved_executable.has_parent_path()) {
        const auto directory = resolved_executable.parent_path();
        candidates.push_back(directory / filename);
#ifndef _WIN32
        candidates.push_back(directory.parent_path() / "lib" / "discord-music-bot" /
                             filename);
#endif
    }
#ifndef _WIN32
    candidates.emplace_back(std::filesystem::path("/usr/lib/discord-music-bot") / filename);
#endif
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return filename;
}

std::filesystem::path prepare_cookie_jar(const std::filesystem::path& source) {
#ifndef _WIN32
    const auto cache_home = system_environment("XDG_CACHE_HOME");
    if (!cache_home.empty()) {
        std::error_code error;
        auto directory = std::filesystem::absolute(cache_home, error).lexically_normal();
        if (error) {
            throw ConfigError("cannot resolve YouTube cookie cache: " + error.message());
        }
        const auto destination = directory / "youtube-cookies.txt";
        if (source != destination) {
            std::filesystem::create_directories(directory, error);
            if (error) {
                throw ConfigError("cannot create YouTube cookie cache: " + error.message());
            }
            std::filesystem::copy_file(
                source, destination, std::filesystem::copy_options::overwrite_existing,
                error);
            if (error) {
                throw ConfigError("cannot create writable YouTube cookie jar: " +
                                  error.message());
            }
            std::filesystem::permissions(
                destination,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace,
                error);
            if (error) {
                throw ConfigError("cannot protect YouTube cookie jar: " + error.message());
            }
            return destination;
        }
    }
#endif
    return source;
}

}  // namespace

Config Config::load(const std::filesystem::path& executable_path) {
    std::vector<std::filesystem::path> candidates;
    if (executable_path.has_parent_path()) {
        candidates.push_back(std::filesystem::absolute(executable_path).parent_path() / ".env");
    }
    candidates.push_back(std::filesystem::current_path() / ".env");
#ifndef _WIN32
    candidates.emplace_back("/etc/discord-music-bot/config.env");
#endif
    const auto user_path = user_config_path();
    if (!user_path.empty()) {
        candidates.push_back(user_path);
    }

    EnvironmentFile values;
    std::unordered_set<std::string> visited;
    std::optional<std::filesystem::path> configuration_path;
    for (const auto& candidate : candidates) {
        const auto key = candidate.lexically_normal().string();
        if (visited.insert(key).second) {
            if (load_env_file(candidate, values) && !configuration_path) {
                configuration_path = std::filesystem::absolute(candidate);
                break;
            }
        }
    }

    Config result;
    result.token = config_value(values, "DISCORD_TOKEN");
    if (result.token.empty() || result.token == "replace_me") {
        throw ConfigError("set DISCORD_TOKEN in the configuration file");
    }

    const auto guild = config_value(values, "DISCORD_GUILD_ID");
    if (!guild.empty()) {
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(guild.data(), guild.data() + guild.size(), value);
        if (error != std::errc{} || end != guild.data() + guild.size() || value == 0) {
            throw ConfigError("DISCORD_GUILD_ID must be a positive Discord ID");
        }
        result.guild_id = value;
    }

    result.default_volume_percent =
        parse_integer<int>(values, "DEFAULT_VOLUME", 50, 0, 200);
    result.max_track_duration =
        parse_integer<std::int64_t>(values, "MAX_TRACK_DURATION", 10800, 0);
    result.idle_timeout_seconds =
        parse_integer<int>(values, "IDLE_TIMEOUT", 300, 30);
    result.buffer_ahead_seconds =
        parse_integer<int>(values, "BUFFER_AHEAD_SECONDS", 60, 5, 3600);
    result.prebuffer_seconds =
        parse_integer<int>(values, "PREBUFFER_SECONDS", 2, 0,
                           result.buffer_ahead_seconds);
    result.configuration_path = configuration_path;

    if (!config_value(values, "YOUTUBE_COOKIES_FROM_BROWSER").empty()) {
        throw ConfigError(
            "YOUTUBE_COOKIES_FROM_BROWSER is unavailable for the Ubuntu service; "
            "export a Netscape cookie file and set YOUTUBE_COOKIES_FILE");
    }
    const auto cookies = config_value(values, "YOUTUBE_COOKIES_FILE");
    if (!cookies.empty()) {
        auto path = std::filesystem::path(cookies);
        if (path.is_relative()) {
            path = configuration_path ? configuration_path->parent_path() / path
                                      : std::filesystem::current_path() / path;
        }
        path = std::filesystem::absolute(path).lexically_normal();
        if (!std::filesystem::is_regular_file(path)) {
            throw ConfigError("YOUTUBE_COOKIES_FILE not found: " + path.string());
        }
        result.youtube_cookies_file = prepare_cookie_jar(path);
    }
    const auto configured_yt_dlp = config_value(values, "YT_DLP_PATH");
    if (!configured_yt_dlp.empty()) {
        result.yt_dlp_path = configured_yt_dlp;
    } else {
#ifdef _WIN32
        result.yt_dlp_path = default_helper_path(executable_path, "yt-dlp.exe");
#else
        result.yt_dlp_path = default_helper_path(executable_path, "yt-dlp");
#endif
    }
    const auto configured_deno = config_value(values, "DENO_PATH");
    if (!configured_deno.empty()) {
        result.deno_path = configured_deno;
    } else {
#ifdef _WIN32
        result.deno_path = default_helper_path(executable_path, "deno.exe");
#else
        result.deno_path = default_helper_path(executable_path, "deno");
#endif
    }
    return result;
}

}  // namespace musicbot
