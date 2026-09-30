#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace musicbot {

struct Track {
    std::string title;
    std::string webpage_url;
    std::optional<std::int64_t> duration_seconds;
    std::uint64_t requester_id{};
    std::string requester_name;
    std::uint64_t text_channel_id{};
    std::string thumbnail;
    bool is_live{};
};

struct MediaStream {
    std::string url;
    std::vector<std::string> headers;
};

inline std::string format_duration(const Track& track) {
    if (track.is_live) {
        return "прямой эфир";
    }
    if (!track.duration_seconds) {
        return "неизвестно";
    }
    auto seconds = std::max<std::int64_t>(0, *track.duration_seconds);
    const auto hours = seconds / 3600;
    seconds %= 3600;
    const auto minutes = seconds / 60;
    seconds %= 60;
    char value[32]{};
    if (hours > 0) {
        std::snprintf(value, sizeof(value), "%lld:%02lld:%02lld",
                      static_cast<long long>(hours), static_cast<long long>(minutes),
                      static_cast<long long>(seconds));
    } else {
        std::snprintf(value, sizeof(value), "%lld:%02lld",
                      static_cast<long long>(minutes), static_cast<long long>(seconds));
    }
    return value;
}

}  // namespace musicbot
