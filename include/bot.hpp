#pragma once

#include "config.hpp"
#include "player.hpp"
#include "youtube.hpp"

#include <dpp/dpp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace musicbot {

class UserError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MusicBot {
public:
    explicit MusicBot(Config config);
    ~MusicBot();

    MusicBot(const MusicBot&) = delete;
    MusicBot& operator=(const MusicBot&) = delete;

    void run();

private:
    struct AsyncTask {
        std::shared_ptr<std::atomic_bool> done;
        std::jthread thread;
    };

    void register_commands();
    void handle_command(const dpp::slashcommand_t& event);
    void handle_play(const dpp::slashcommand_t& event, const std::string& query);
    void ensure_voice(const dpp::slashcommand_t& event);
    dpp::discord_voice_client* require_same_voice(const dpp::slashcommand_t& event);
    std::uint64_t member_voice_channel(const dpp::slashcommand_t& event) const;
    GuildPlayer& player(const dpp::slashcommand_t& event);
    void launch_task(std::function<void(std::stop_token)> task);
    static void reply_error(const dpp::slashcommand_t& event, const std::string& message);

    Config config_;
    dpp::cluster bot_;
    YouTubeExtractor extractor_;
    PlayerManager players_;
    std::atomic_bool shutting_down_{};
    std::mutex tasks_mutex_;
    std::vector<AsyncTask> tasks_;
};

}  // namespace musicbot
