#include "musicbot/bot.hpp"
#include "musicbot/config.hpp"
#include "musicbot/youtube.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

int main(int argc, char** argv) {
    try {
        std::clog << std::unitbuf;
        if (argc > 1 && std::string_view(argv[1]) == "--version") {
            std::cout << "discord-music-bot 2.0.0\n";
            return 0;
        }
        const auto executable = argc > 0 ? std::filesystem::path(argv[0])
                                         : std::filesystem::path("DiscordMusicBot");
        auto config = musicbot::Config::load(executable);
        musicbot::YouTubeExtractor extractor(config);
        std::string version;
        if (!extractor.check_available(version)) {
            std::cerr << "Configuration error: YouTube helpers are unavailable: "
                      << version << '\n';
            return 2;
        }
        std::clog << "Configuration: "
                  << (config.configuration_path ? config.configuration_path->string()
                                                : "environment variables") << '\n'
                  << "Media engine: native HTTP + WebM/Opus (no FFmpeg/GStreamer)\n"
                  << "YouTube helpers: " << version << '\n'
                  << "Buffer ahead: " << config.buffer_ahead_seconds << " seconds\n"
                  << "Startup prebuffer: " << config.prebuffer_seconds << " seconds\n"
                  << "YouTube cookies: "
                  << (config.youtube_cookies_file ? config.youtube_cookies_file->string()
                                                  : "not configured") << '\n';
        musicbot::MusicBot bot(std::move(config));
        bot.run();
        return 0;
    } catch (const musicbot::ConfigError& error) {
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
