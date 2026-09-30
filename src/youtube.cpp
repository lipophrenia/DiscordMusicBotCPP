#include "musicbot/youtube.hpp"

#include "musicbot/process.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <ranges>
#include <string_view>
#include <vector>

namespace musicbot {
namespace {

constexpr std::string_view kOpusFormat =
    "bestaudio[ext=webm][acodec=opus]/bestaudio[acodec=opus]";

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool begins_with_http(const std::string& value) {
    std::string prefix = value.substr(0, std::min<std::size_t>(8, value.size()));
    std::ranges::transform(prefix, prefix.begin(),
                           [](unsigned char character) { return std::tolower(character); });
    return prefix.starts_with("http://") || prefix.starts_with("https://");
}

bool is_youtube_url(const std::string& value) {
    if (!begins_with_http(value)) {
        return false;
    }
    const auto scheme_end = value.find("://");
    const auto host_begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const auto host_end = value.find_first_of("/:?#", host_begin);
    auto host = value.substr(host_begin, host_end - host_begin);
    std::ranges::transform(host, host.begin(),
                           [](unsigned char character) { return std::tolower(character); });
    if (const auto port = host.find(':'); port != std::string::npos) {
        host.erase(port);
    }
    return host == "youtube.com" || host == "www.youtube.com" ||
           host == "m.youtube.com" || host == "music.youtube.com" || host == "youtu.be";
}

const nlohmann::json& first_item(const nlohmann::json& root) {
    if (!root.contains("entries")) {
        return root;
    }
    const auto& entries = root.at("entries");
    if (entries.is_array()) {
        for (const auto& entry : entries) {
            if (entry.is_object()) {
                return entry;
            }
        }
    }
    throw ExtractionError("По запросу ничего не найдено.");
}

std::string json_string(const nlohmann::json& value, const char* key) {
    if (const auto iterator = value.find(key);
        iterator != value.end() && iterator->is_string()) {
        return iterator->get<std::string>();
    }
    return {};
}

std::string process_error(const ProcessResult& result) {
    auto message = trim(result.error_output);
    if (message.size() > 600) {
        message.resize(600);
        message += "…";
    }
    return message;
}

void append_common_arguments(
    std::vector<std::string>& arguments,
    const Config& config) {
    arguments.insert(arguments.end(), {
        "--quiet", "--no-warnings", "--no-progress", "--no-playlist",
        "--format", std::string(kOpusFormat),
        "--js-runtimes", "deno:" + config.deno_path.string(),
    });
    if (config.youtube_cookies_file) {
        arguments.emplace_back("--cookies");
        arguments.push_back(config.youtube_cookies_file->string());
    }
}

std::string selected_media_url(const nlohmann::json& item) {
    auto url = json_string(item, "url");
    if (!url.empty()) {
        return url;
    }
    const auto downloads = item.find("requested_downloads");
    if (downloads != item.end() && downloads->is_array()) {
        for (const auto& download : *downloads) {
            url = json_string(download, "url");
            if (!url.empty()) {
                return url;
            }
        }
    }
    return {};
}

const nlohmann::json* selected_download(const nlohmann::json& item) {
    const auto downloads = item.find("requested_downloads");
    if (downloads != item.end() && downloads->is_array()) {
        for (const auto& download : *downloads) {
            if (download.is_object() && !json_string(download, "url").empty()) {
                return &download;
            }
        }
    }
    return &item;
}

std::vector<std::string> media_headers(const nlohmann::json& item) {
    std::vector<std::string> result;
    const auto* selected = selected_download(item);
    auto headers = selected->find("http_headers");
    if (headers == selected->end() || !headers->is_object()) {
        headers = item.find("http_headers");
        if (headers == item.end() || !headers->is_object()) {
            return result;
        }
    }
    for (const auto& [name, value] : headers->items()) {
        if (value.is_string() && name.find_first_of("\r\n:") == std::string::npos) {
            auto text = value.get<std::string>();
            if (text.find_first_of("\r\n") == std::string::npos) {
                result.push_back(name + ": " + text);
            }
        }
    }
    return result;
}

}  // namespace

YouTubeExtractor::YouTubeExtractor(const Config& config) : config_(config) {}

Track YouTubeExtractor::create_track(
    const std::string& raw_query,
    std::uint64_t requester_id,
    std::string requester_name,
    std::uint64_t text_channel_id,
    std::stop_token stop_token) const {
    const auto query = trim(raw_query);
    if (query.empty()) {
        throw ExtractionError("Укажите ссылку на YouTube или поисковый запрос.");
    }
    if (query.size() > 500) {
        throw ExtractionError("Ссылка или поисковый запрос слишком длинные.");
    }
    if (begins_with_http(query) && !is_youtube_url(query)) {
        throw ExtractionError("Поддерживаются только ссылки YouTube.");
    }
    const auto target = begins_with_http(query) ? query : "ytsearch1:" + query;
    std::vector<std::string> arguments{"--dump-single-json", "--skip-download"};
    append_common_arguments(arguments, config_);
    arguments.insert(arguments.end(), {"--", target});
    auto result = capture_process(config_.yt_dlp_path, arguments, 16U * 1024U * 1024U,
                                  stop_token);
    if (result.cancelled || stop_token.stop_requested()) {
        throw ExtractionError("Получение данных отменено.");
    }
    if (result.exit_code != 0) {
        const auto details = process_error(result);
        throw ExtractionError(details.empty() ? "yt-dlp не смог открыть видео."
                                              : "yt-dlp: " + details);
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(result.output.begin(), result.output.end());
    } catch (const nlohmann::json::exception&) {
        throw ExtractionError("yt-dlp вернул некорректные метаданные.");
    }
    const auto& item = first_item(root);
    auto webpage_url = json_string(item, "webpage_url");
    if (webpage_url.empty()) {
        webpage_url = json_string(item, "original_url");
    }
    if (!is_youtube_url(webpage_url)) {
        throw ExtractionError("Не удалось получить ссылку на видео YouTube.");
    }

    Track track;
    track.title = json_string(item, "title");
    if (track.title.empty()) {
        track.title = "Без названия";
    }
    track.webpage_url = std::move(webpage_url);
    if (const auto duration = item.find("duration");
        duration != item.end() && duration->is_number()) {
        track.duration_seconds = static_cast<std::int64_t>(duration->get<double>());
    }
    if (config_.max_track_duration > 0 && track.duration_seconds &&
        *track.duration_seconds > config_.max_track_duration) {
        throw ExtractionError("Видео превышает настроенный лимит длительности.");
    }
    track.thumbnail = json_string(item, "thumbnail");
    track.is_live = item.value("is_live", false);
    track.requester_id = requester_id;
    track.requester_name = std::move(requester_name);
    track.text_channel_id = text_channel_id;
    std::clog << "Media metadata loaded: " << track.title << " ("
              << format_duration(track) << ")\n";
    return track;
}

MediaStream YouTubeExtractor::resolve_stream(
    const std::string& webpage_url,
    std::stop_token stop_token) const {
    std::vector<std::string> arguments{"--dump-single-json", "--skip-download"};
    append_common_arguments(arguments, config_);
    arguments.insert(arguments.end(), {"--", webpage_url});
    const auto result = capture_process(config_.yt_dlp_path, arguments,
                                        16U * 1024U * 1024U, stop_token);
    if (result.cancelled || stop_token.stop_requested()) {
        return {};
    }
    if (result.exit_code != 0) {
        const auto details = process_error(result);
        throw ExtractionError(details.empty() ? "yt-dlp не смог получить URL потока WebM/Opus."
                                              : "yt-dlp: " + details);
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(result.output.begin(), result.output.end());
    } catch (const nlohmann::json::exception&) {
        throw ExtractionError("yt-dlp вернул некорректное описание медиапотока.");
    }
    const auto& item = first_item(root);
    auto url = selected_media_url(item);
    if (!begins_with_http(url)) {
        throw ExtractionError("yt-dlp не вернул прямой URL медиапотока.");
    }
    return MediaStream{std::move(url), media_headers(item)};
}

bool YouTubeExtractor::check_available(std::string& details) const {
    try {
        const auto result = capture_process(config_.yt_dlp_path, {"--version"}, 4096);
        std::string yt_dlp_version(result.output.begin(), result.output.end());
        yt_dlp_version = trim(yt_dlp_version);
        if (result.exit_code != 0) {
            details = "yt-dlp is unavailable";
            if (const auto error = process_error(result); !error.empty()) {
                details += ": " + error;
            }
            return false;
        }
        const auto deno = capture_process(config_.deno_path, {"--version"}, 16U * 1024U);
        std::string deno_version(deno.output.begin(), deno.output.end());
        deno_version = trim(deno_version);
        if (const auto newline = deno_version.find('\n'); newline != std::string::npos) {
            deno_version.resize(newline);
        }
        if (deno.exit_code != 0 || deno_version.empty()) {
            details = "Deno is unavailable";
            if (const auto error = process_error(deno); !error.empty()) {
                details += ": " + error;
            }
            return false;
        }
        details = "yt-dlp " + yt_dlp_version + ", " + deno_version;
        return !yt_dlp_version.empty();
    } catch (const std::exception& error) {
        details = error.what();
        return false;
    }
}

}  // namespace musicbot
