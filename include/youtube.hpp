#pragma once

#include "config.hpp"
#include "models.hpp"

#include <stdexcept>
#include <stop_token>
#include <string>

namespace musicbot {

class ExtractionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class YouTubeExtractor {
public:
    explicit YouTubeExtractor(const Config& config);

    Track create_track(
        const std::string& query,
        std::uint64_t requester_id,
        std::string requester_name,
        std::uint64_t text_channel_id,
        std::stop_token stop_token = {}) const;

    [[nodiscard]] MediaStream resolve_stream(
        const std::string& webpage_url,
        std::stop_token stop_token = {}) const;

    [[nodiscard]] bool check_available(std::string& details) const;

private:
    const Config& config_;
};

}  // namespace musicbot
