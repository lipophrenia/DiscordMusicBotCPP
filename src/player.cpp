#include "player.hpp"

#include "opus_volume.hpp"
#include "webm_opus.hpp"
#include "http_stream.hpp"
#include "local_audio.hpp"
#include "startup_packet_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace musicbot {
namespace {

constexpr auto kMediaStreamCacheLifetime = std::chrono::minutes(30);

}  // namespace

GuildPlayer::GuildPlayer(
    dpp::cluster& bot,
    const Config& config,
    const YouTubeExtractor& extractor,
    std::uint64_t guild_id,
    dpp::discord_client* shard)
    : bot_(bot),
      config_(config),
      extractor_(extractor),
      guild_id_(guild_id),
      shard_(shard),
      volume_percent_(config.default_volume_percent),
      worker_([this](std::stop_token token) { run(token); }) {}

GuildPlayer::~GuildPlayer() {
    shutdown();
}

std::size_t GuildPlayer::enqueue(Track track, dpp::discord_client* shard) {
    shard_.store(shard, std::memory_order_release);
    std::scoped_lock lock(mutex_);
    const auto position = queue_.size() + (current_ ? 1U : 0U) + 1U;
    queue_.push_back(std::move(track));
    condition_.notify_one();
    return position;
}

PlayerSnapshot GuildPlayer::snapshot(std::size_t maximum_queued) const {
    std::scoped_lock lock(mutex_);
    PlayerSnapshot result;
    result.current = current_;
    result.total_queued = queue_.size();
    const auto count = std::min(maximum_queued, queue_.size());
    result.queued.reserve(count);
    auto iterator = queue_.begin();
    for (std::size_t index = 0; index < count; ++index, ++iterator) {
        result.queued.push_back(*iterator);
    }
    return result;
}

std::optional<Track> GuildPlayer::current_track() const {
    std::scoped_lock lock(mutex_);
    return current_;
}

bool GuildPlayer::busy() const {
    std::scoped_lock lock(mutex_);
    return current_.has_value() || !queue_.empty();
}

bool GuildPlayer::skip() {
    {
        std::scoped_lock lock(mutex_);
        if (!current_) {
            return false;
        }
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (auto* voice = voice_client()) {
        voice->stop_audio();
    }
    return true;
}

std::size_t GuildPlayer::stop() {
    std::size_t removed{};
    {
        std::scoped_lock lock(mutex_);
        removed = queue_.size();
        queue_.clear();
        if (current_) {
            generation_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (auto* voice = voice_client()) {
        voice->stop_audio();
    }
    return removed;
}

void GuildPlayer::set_paused(bool paused) {
    auto* voice = voice_client();
    if (voice == nullptr || !busy()) {
        throw std::runtime_error("nothing is currently playing");
    }
    voice->pause_audio(paused);
    paused_.store(paused, std::memory_order_release);
}

bool GuildPlayer::paused() const {
    return paused_.load(std::memory_order_acquire);
}

void GuildPlayer::set_volume(int percent) {
    volume_percent_.store(std::clamp(percent, 0, 200), std::memory_order_release);
}

int GuildPlayer::volume() const noexcept {
    return volume_percent_.load(std::memory_order_acquire);
}

void GuildPlayer::shutdown() {
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        queue_.clear();
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    worker_.request_stop();
    condition_.notify_all();
    if (auto* voice = voice_client()) {
        voice->stop_audio();
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void GuildPlayer::run(std::stop_token stop_token) {
    using namespace std::chrono_literals;
    while (!stop_token.stop_requested()) {
        const Track* track{};
        std::uint64_t generation{};
        {
            std::unique_lock lock(mutex_);
            const auto ready = condition_.wait_for(
                lock, std::chrono::seconds(config_.idle_timeout_seconds),
                [&] { return shutdown_ || !queue_.empty(); });
            if (shutdown_ || stop_token.stop_requested()) {
                return;
            }
            if (!ready) {
                auto* shard = shard_.load(std::memory_order_acquire);
                lock.unlock();
                if (shard != nullptr) {
                    shard->disconnect_voice(dpp::snowflake(guild_id_));
                }
                continue;
            }
            current_.emplace(std::move(queue_.front()));
            queue_.pop_front();
            track = &*current_;
            paused_.store(false, std::memory_order_release);
            generation = generation_.load(std::memory_order_acquire);
        }

        try {
            const auto voice_wait_started = std::chrono::steady_clock::now();
            if (!wait_for_voice(generation, stop_token)) {
                throw std::runtime_error("voice connection is not ready");
            }
            const auto voice_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - voice_wait_started).count();
            std::clog << "Voice ready: " << track->title << " (waited " << voice_wait_ms << " ms)\n";
            announce(track->text_channel_id,
                     "▶️ Сейчас играет: **" + track->title + "** (`" +
                         format_duration(*track) + "`), добавил " + track->requester_name);
            play_track(*track, generation, stop_token);
        } catch (const std::exception& error) {
            if (!stop_token.stop_requested() &&
                generation == generation_.load(std::memory_order_acquire)) {
                std::cerr << "Playback error in guild " << guild_id_ << ": " << error.what()
                          << '\n';
                announce(track->text_channel_id,
                         "⚠️ Не удалось воспроизвести **" + track->title +
                             "**; перехожу к следующему треку.");
            }
        }

        {
            std::scoped_lock lock(mutex_);
            current_.reset();
            paused_.store(false, std::memory_order_release);
        }
    }
}

void GuildPlayer::play_track(
    const Track& track,
    std::uint64_t generation,
    std::stop_token stop_token) {
    using namespace std::chrono_literals;
    std::optional<OpusVolume> volume_processor;
    const bool local = track.local_filename.has_value();
    StartupPacketBuffer startup_packets(local ? 0 : config_.prebuffer_seconds);
    const auto playback_started = std::chrono::steady_clock::now();
    bool first_packet = true;

    const auto send_packet = [&](std::span<const std::uint8_t> packet) {
        if (stop_token.stop_requested() ||
            generation != generation_.load(std::memory_order_acquire)) {
            return false;
        }
        auto* voice = voice_client();
        if (voice == nullptr || !voice->is_ready()) {
            throw std::runtime_error("voice connection lost");
        }
        // Local files enqueue as fast as they are read; only network streams
        // limit download lead. DPP handles playback timing for both sources.
        while (!local && voice->get_secs_remaining() >
                             static_cast<float>(config_.buffer_ahead_seconds)) {
            if (stop_token.stop_requested() ||
                generation != generation_.load(std::memory_order_acquire)) {
                return false;
            }
            std::this_thread::sleep_for(20ms);
            voice = voice_client();
            if (voice == nullptr || !voice->is_ready()) {
                throw std::runtime_error("voice connection lost");
            }
        }
        // skip/stop may arrive while waiting for queue space.
        if (stop_token.stop_requested() ||
            generation != generation_.load(std::memory_order_acquire)) return false;
        voice->send_audio_opus(packet.data(), packet.size());
        if (first_packet) {
            first_packet = false;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - playback_started).count();
            std::clog << (local ? "Local" : "Network") << " media first packet queued: "
                      << track.title << " (" << elapsed << " ms after voice ready)\n";
        }
        return true;
    };

    const auto log_prebuffer = [&] {
        std::clog << "Media prebuffer ready: " << track.title << " ("
                  << startup_packets.seconds() << " seconds)\n";
    };

    WebmOpusDemuxer* demuxer_pointer{};
    WebmOpusDemuxer demuxer([&](std::span<const std::uint8_t> packet) {
        if (stop_token.stop_requested() ||
            generation != generation_.load(std::memory_order_acquire)) {
            return false;
        }
        auto outgoing = packet;
        const auto current_volume = volume();
        if (current_volume != 100) {
            if (!volume_processor) {
                volume_processor.emplace(demuxer_pointer->opus_channels());
            }
            outgoing = volume_processor->process(packet, current_volume);
        }
        const bool was_ready = startup_packets.ready();
        const bool sent = startup_packets.feed(outgoing, send_packet);
        if (sent && !was_ready && startup_packets.ready()) log_prebuffer();
        return sent;
    });
    demuxer_pointer = &demuxer;

    if (track.local_filename) {
        std::clog << "Local media reading: " << track.title << '\n';
        LocalAudioLibrary library(config_.local_audio_dir);
        if (!library.stream(*track.local_filename,
                [&](std::span<const std::uint8_t> chunk) {
                    if (stop_token.stop_requested() ||
                        generation != generation_.load(std::memory_order_acquire)) return false;
                    return demuxer.feed(chunk);
                }, stop_token)) return;
    } else {
        MediaStream refreshed_stream;
        const MediaStream* stream = track.media_stream.get();
        const auto stream_age = std::chrono::steady_clock::now() - track.media_stream_resolved_at;
        if (stream == nullptr || stream_age >= kMediaStreamCacheLifetime) {
            std::clog << "Media stream resolving: " << track.title << '\n';
            refreshed_stream = extractor_.resolve_stream(track.webpage_url, stop_token);
            stream = &refreshed_stream;
        } else {
            std::clog << "Media stream cache hit: " << track.title << '\n';
        }
        if (stream->url.empty() || generation != generation_.load(std::memory_order_acquire) ||
            stop_token.stop_requested()) {
            return;
        }
        std::clog << "Media download started: " << track.title << '\n';
        stream_http(
            stream->url,
            [&](std::span<const std::uint8_t> chunk) { return demuxer.feed(chunk); },
            stream->headers,
            config_.youtube_cookies_file,
            stop_token);
    }
    if (generation != generation_.load(std::memory_order_acquire) ||
        stop_token.stop_requested()) {
        return;
    }
    demuxer.finish();
    if (!startup_packets.ready()) {
        if (!startup_packets.finish(send_packet)) return;
        log_prebuffer();
    }
    std::clog << "Media reading completed: " << track.title << '\n';

    while (generation == generation_.load(std::memory_order_acquire) &&
           !stop_token.stop_requested()) {
        auto* voice = voice_client();
        if (voice == nullptr || voice->get_secs_remaining() <= 0.05F) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
}

dpp::discord_voice_client* GuildPlayer::voice_client() const {
    auto* shard = shard_.load(std::memory_order_acquire);
    if (shard == nullptr) {
        return nullptr;
    }
    auto* connection = shard->get_voice(dpp::snowflake(guild_id_));
    return connection == nullptr ? nullptr : connection->voiceclient.get();
}

bool GuildPlayer::wait_for_voice(
    std::uint64_t generation,
    std::stop_token stop_token) const {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop_token.stop_requested() ||
            generation != generation_.load(std::memory_order_acquire)) {
            return false;
        }
        if (auto* voice = voice_client(); voice != nullptr && voice->is_ready()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

void GuildPlayer::announce(std::uint64_t channel_id, const std::string& message) const {
    dpp::message announcement(dpp::snowflake(channel_id), message);
    announcement.set_allowed_mentions(false, false, false, false);
    bot_.message_create(announcement);
}

PlayerManager::PlayerManager(
    dpp::cluster& bot,
    const Config& config,
    const YouTubeExtractor& extractor)
    : bot_(bot), config_(config), extractor_(extractor) {}

PlayerManager::~PlayerManager() {
    shutdown();
}

GuildPlayer& PlayerManager::get(std::uint64_t guild_id, dpp::discord_client* shard) {
    std::scoped_lock lock(mutex_);
    auto& player = players_[guild_id];
    if (!player) {
        player = std::make_unique<GuildPlayer>(bot_, config_, extractor_, guild_id, shard);
    }
    return *player;
}

void PlayerManager::remove(std::uint64_t guild_id) {
    std::unique_ptr<GuildPlayer> player;
    {
        std::scoped_lock lock(mutex_);
        const auto iterator = players_.find(guild_id);
        if (iterator == players_.end()) {
            return;
        }
        player = std::move(iterator->second);
        players_.erase(iterator);
    }
    player->shutdown();
}

void PlayerManager::shutdown() {
    std::unordered_map<std::uint64_t, std::unique_ptr<GuildPlayer>> players;
    {
        std::scoped_lock lock(mutex_);
        players.swap(players_);
    }
    for (auto& [guild_id, player] : players) {
        static_cast<void>(guild_id);
        player->shutdown();
    }
}

}  // namespace musicbot
