#pragma once

#include "config.hpp"
#include "models.hpp"
#include "youtube.hpp"

#include <dpp/dpp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace musicbot {

struct PlayerSnapshot {
    std::optional<Track> current;
    std::vector<Track> queued;
    std::size_t total_queued{};
};

class GuildPlayer {
public:
    GuildPlayer(
        dpp::cluster& bot,
        const Config& config,
        const YouTubeExtractor& extractor,
        std::uint64_t guild_id,
        dpp::discord_client* shard);
    ~GuildPlayer();

    GuildPlayer(const GuildPlayer&) = delete;
    GuildPlayer& operator=(const GuildPlayer&) = delete;

    std::size_t enqueue(Track track, dpp::discord_client* shard);
    [[nodiscard]] PlayerSnapshot snapshot(std::size_t maximum_queued) const;
    [[nodiscard]] std::optional<Track> current_track() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool skip();
    std::size_t stop();
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const;
    void set_volume(int percent);
    [[nodiscard]] int volume() const noexcept;
    void shutdown();

private:
    void run(std::stop_token stop_token);
    void play_track(const Track& track, std::uint64_t generation, std::stop_token stop_token);
    dpp::discord_voice_client* voice_client() const;
    bool wait_for_voice(std::uint64_t generation, std::stop_token stop_token) const;
    void announce(std::uint64_t channel_id, const std::string& message) const;

    dpp::cluster& bot_;
    const Config& config_;
    const YouTubeExtractor& extractor_;
    std::uint64_t guild_id_{};
    std::atomic<dpp::discord_client*> shard_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Track> queue_;
    std::optional<Track> current_;
    std::atomic<std::uint64_t> generation_{};
    std::atomic<int> volume_percent_{};
    std::atomic_bool paused_{};
    bool shutdown_{};
    std::jthread worker_;
};

class PlayerManager {
public:
    PlayerManager(dpp::cluster& bot, const Config& config, const YouTubeExtractor& extractor);
    ~PlayerManager();

    GuildPlayer& get(std::uint64_t guild_id, dpp::discord_client* shard);
    void remove(std::uint64_t guild_id);
    void shutdown();

private:
    dpp::cluster& bot_;
    const Config& config_;
    const YouTubeExtractor& extractor_;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<GuildPlayer>> players_;
};

}  // namespace musicbot
