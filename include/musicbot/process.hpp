#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace musicbot {

struct ProcessResult {
    int exit_code{-1};
    std::vector<std::uint8_t> output;
    std::string error_output;
    bool cancelled{};
};

using OutputHandler = std::function<bool(std::span<const std::uint8_t>)>;

ProcessResult stream_process(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const OutputHandler& output_handler,
    std::stop_token stop_token = {});

ProcessResult capture_process(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    std::size_t maximum_output_bytes = 16U * 1024U * 1024U,
    std::stop_token stop_token = {});

}  // namespace musicbot
