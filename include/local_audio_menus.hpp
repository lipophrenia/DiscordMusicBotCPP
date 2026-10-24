#pragma once

#include "local_audio.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace musicbot {

// A button refers to the filenames that were displayed, not a live directory index.
// Discord interaction IDs keep old buttons distinct across process restarts.
class LocalAudioMenus {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto lifetime = std::chrono::minutes(15);
    static constexpr std::size_t capacity = 512;

    void remember(std::uint64_t interaction_id, std::uint64_t user_id,
                  std::uint64_t guild_id, std::vector<std::string> filenames,
                  Clock::time_point now = Clock::now());
    [[nodiscard]] std::string select(std::uint64_t interaction_id, std::size_t index,
                                    std::uint64_t user_id, std::uint64_t guild_id,
                                    Clock::time_point now = Clock::now());

private:
    struct Menu {
        std::uint64_t user_id;
        std::uint64_t guild_id;
        std::vector<std::string> filenames;
        Clock::time_point created;
    };
    std::mutex mutex_;
    std::map<std::uint64_t, Menu> menus_;
};

} // namespace musicbot
