#pragma once

#include "models.hpp"
#include "process.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace musicbot {

class LocalAudioError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LocalAudioPage {
    std::vector<std::string> filenames;
    std::size_t total_files{};
    std::size_t total_pages{};
};

class LocalAudioLibrary {
public:
    static constexpr std::size_t page_size = 5;
    explicit LocalAudioLibrary(std::filesystem::path directory);

    [[nodiscard]] LocalAudioPage list(std::size_t page = 1) const;
    Track create_track(const std::string& filename, std::uint64_t requester_id,
                       std::string requester_name, std::uint64_t text_channel_id) const;
    // Returns false on cancellation; the callback supplies playback backpressure.
    bool stream(const std::string& filename, const OutputHandler& output,
                std::stop_token stop = {}) const;

private:
    [[nodiscard]] std::filesystem::path resolve(const std::string& filename) const;
    std::filesystem::path directory_;
};

}  // namespace musicbot
