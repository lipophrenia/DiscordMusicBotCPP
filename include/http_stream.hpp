#pragma once

#include "process.hpp"

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <stdexcept>

namespace musicbot {

class HttpError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void stream_http(
    const std::string& url,
    const OutputHandler& output_handler,
    const std::vector<std::string>& headers = {},
    const std::optional<std::filesystem::path>& cookies_file = std::nullopt,
    std::stop_token stop_token = {});

}  // namespace musicbot
