#include "http_stream.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace musicbot {
namespace {

struct TransferContext {
    const OutputHandler& handler;
    std::stop_token stop_token;
    std::exception_ptr callback_error;
    curl_off_t received_bytes{};
    bool handler_stopped{};
};

constexpr unsigned kMaximumConsecutiveRetries = 4;

bool is_transient_error(CURLcode error) {
    switch (error) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_PARTIAL_FILE:
            return true;
        default:
            return false;
    }
}

bool wait_before_retry(std::stop_token stop_token, unsigned failures) {
    using namespace std::chrono_literals;
    const auto delay = 250ms * (1U << std::min(failures, 3U));
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop_token.stop_requested()) {
            return false;
        }
        std::this_thread::sleep_for(50ms);
    }
    return true;
}

void initialize_curl() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw HttpError("curl_global_init failed");
        }
    });
}

std::size_t write_callback(char* data, std::size_t size, std::size_t count, void* opaque) {
    auto& context = *static_cast<TransferContext*>(opaque);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context.callback_error =
            std::make_exception_ptr(HttpError("media response chunk size overflow"));
        return 0;
    }
    const auto bytes = size * count;
    if (context.stop_token.stop_requested()) {
        context.handler_stopped = true;
        return 0;
    }
    try {
        if (!context.handler(std::span(
                reinterpret_cast<const std::uint8_t*>(data), bytes))) {
            context.handler_stopped = true;
            return 0;
        }
        if (bytes > static_cast<std::size_t>(
                        std::numeric_limits<curl_off_t>::max() - context.received_bytes)) {
            throw HttpError("media response size overflow");
        }
        context.received_bytes += static_cast<curl_off_t>(bytes);
    } catch (...) {
        context.callback_error = std::current_exception();
        return 0;
    }
    return bytes;
}

int progress_callback(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& context = *static_cast<const TransferContext*>(opaque);
    return context.stop_token.stop_requested() || context.handler_stopped ? 1 : 0;
}

void require_option(CURLcode result, const char* option) {
    if (result != CURLE_OK) {
        throw HttpError(std::string("failed to set curl option ") + option);
    }
}

}  // namespace

void stream_http(
    const std::string& url,
    const OutputHandler& output_handler,
    const std::vector<std::string>& headers,
    const std::optional<std::filesystem::path>& cookies_file,
    std::stop_token stop_token) {
    initialize_curl();
    auto* handle = curl_easy_init();
    if (handle == nullptr) {
        throw HttpError("curl_easy_init failed");
    }
    struct CurlCleanup {
        CURL* handle;
        ~CurlCleanup() { curl_easy_cleanup(handle); }
    } cleanup{handle};
    struct HeaderCleanup {
        curl_slist* value{};
        ~HeaderCleanup() { curl_slist_free_all(value); }
    } request_headers;

    TransferContext context{output_handler, stop_token, {}, 0, false};
    std::array<char, CURL_ERROR_SIZE> error{};
    require_option(curl_easy_setopt(handle, CURLOPT_URL, url.c_str()), "URL");
    require_option(curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L), "FOLLOWLOCATION");
    require_option(curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L), "FAILONERROR");
    require_option(curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L), "MAXREDIRS");
#if LIBCURL_VERSION_NUM >= 0x075500
    require_option(curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https"), "PROTOCOLS");
    require_option(curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https"), "REDIR_PROTOCOLS");
#else
    require_option(curl_easy_setopt(handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS), "PROTOCOLS");
    require_option(curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS),
                   "REDIR_PROTOCOLS");
#endif
    require_option(curl_easy_setopt(handle, CURLOPT_USERAGENT,
                                    "Mozilla/5.0 DiscordMusicBot/2.0"), "USERAGENT");
    require_option(curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L), "CONNECTTIMEOUT");
    require_option(curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L), "LOW_SPEED_LIMIT");
    require_option(curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L), "LOW_SPEED_TIME");
    require_option(curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L), "NOSIGNAL");
    require_option(curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L), "TCP_KEEPALIVE");
    require_option(curl_easy_setopt(handle, CURLOPT_BUFFERSIZE, 256L * 1024L), "BUFFERSIZE");
    require_option(curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback), "WRITEFUNCTION");
    require_option(curl_easy_setopt(handle, CURLOPT_WRITEDATA, &context), "WRITEDATA");
    require_option(curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress_callback),
                   "XFERINFOFUNCTION");
    require_option(curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &context), "XFERINFODATA");
    require_option(curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L), "NOPROGRESS");
    require_option(curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error.data()), "ERRORBUFFER");
    for (const auto& header : headers) {
        auto* updated = curl_slist_append(request_headers.value, header.c_str());
        if (updated == nullptr) {
            throw HttpError("failed to allocate HTTP headers");
        }
        request_headers.value = updated;
    }
    if (request_headers.value != nullptr) {
        require_option(curl_easy_setopt(handle, CURLOPT_HTTPHEADER, request_headers.value),
                       "HTTPHEADER");
    }
    if (cookies_file) {
        const auto path = cookies_file->string();
        require_option(curl_easy_setopt(handle, CURLOPT_COOKIEFILE, path.c_str()), "COOKIEFILE");
    }

    unsigned consecutive_failures{};
    for (;;) {
        error.fill('\0');
        require_option(
            curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE, context.received_bytes),
            "RESUME_FROM_LARGE");
        require_option(
            curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT,
                             context.received_bytes == 0 ? 0L : 1L),
            "FRESH_CONNECT");
        const auto attempt_start = context.received_bytes;
        const auto result = curl_easy_perform(handle);
        if (context.callback_error) {
            std::rethrow_exception(context.callback_error);
        }
        if (context.handler_stopped || stop_token.stop_requested()) {
            return;
        }
        long status{};
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        if (result == CURLE_OK) {
            if (status < 200 || status >= 300) {
                throw HttpError("media server returned HTTP " + std::to_string(status));
            }
            return;
        }

        const auto details = error.front() == '\0' ? curl_easy_strerror(result) : error.data();
        if (!is_transient_error(result)) {
            throw HttpError(std::string("media download failed: ") + details);
        }
        if (context.received_bytes > attempt_start) {
            consecutive_failures = 0;
        } else {
            ++consecutive_failures;
        }
        if (consecutive_failures > kMaximumConsecutiveRetries) {
            throw HttpError(std::string("media download failed after retries: ") + details);
        }
        std::clog << "Media HTTP connection interrupted at byte "
                  << context.received_bytes << "; retrying: " << details << '\n';
        if (!wait_before_retry(stop_token, consecutive_failures)) {
            return;
        }
    }
}

}  // namespace musicbot
