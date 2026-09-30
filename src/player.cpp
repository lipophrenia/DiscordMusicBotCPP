#include "player.hpp"

#include "opus_volume.hpp"
#include "webm_opus.hpp"
#include "http_stream.hpp"

#include <opus/opus.h>

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

class StartupPacketBuffer {
public:
    explicit StartupPacketBuffer(int seconds) {
        const auto duration = static_cast<std::size_t>(std::max(seconds, 0));
        packets_.reserve(std::min(duration * 50U + 1U, std::size_t{4096}));
        bytes_.reserve(std::min(duration * 96U * 1024U, std::size_t{2U * 1024U * 1024U}));
    }

    void push(std::span<const std::uint8_t> packet) {
        packets_.push_back(Packet{bytes_.size(), packet.size()});
        bytes_.insert(bytes_.end(), packet.begin(), packet.end());
    }

    template <typename Sender>
    bool flush(Sender&& sender) {
        for (const auto& packet : packets_) {
            if (!sender(
                    std::span<const std::uint8_t>(bytes_).subspan(packet.offset, packet.size))) {
                return false;
            }
        }
        clear();
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return packets_.empty();
    }

private:
    struct Packet {
        std::size_t offset{};
        std::size_t size{};
    };

    void clear() noexcept {
        packets_.clear();
        bytes_.clear();
    }

    std::vector<Packet> packets_;
    std::vector<std::uint8_t> bytes_;
};

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
            if (!wait_for_voice(generation, stop_token)) {
                throw std::runtime_error("voice connection is not ready");
            }
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
    StartupPacketBuffer startup_packets(config_.prebuffer_seconds);
    double startup_seconds{};
    bool startup_complete = config_.prebuffer_seconds == 0;

    const auto send_packet = [&](std::span<const std::uint8_t> packet) {
        if (stop_token.stop_requested() ||
            generation != generation_.load(std::memory_order_acquire)) {
            return false;
        }
        auto* voice = voice_client();
        if (voice == nullptr || !voice->is_ready()) {
            throw std::runtime_error("voice connection lost");
        }
        while (voice->get_secs_remaining() >
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
        voice->send_audio_opus(packet.data(), packet.size());
        return true;
    };

    const auto flush_startup = [&] {
        const auto had_packets = !startup_packets.empty();
        if (!startup_packets.flush(send_packet)) {
            return false;
        }
        startup_complete = true;
        if (had_packets) {
            std::clog << "Media prebuffer ready: " << track.title << " ("
                      << startup_seconds << " seconds)\n";
        }
        return true;
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
        if (startup_complete) {
            return send_packet(outgoing);
        }
        const auto samples = opus_packet_get_nb_samples(
            outgoing.data(), static_cast<opus_int32>(outgoing.size()), 48000);
        if (samples <= 0) {
            throw std::runtime_error("invalid Opus packet duration");
        }
        startup_packets.push(outgoing);
        startup_seconds += static_cast<double>(samples) / 48000.0;
        return startup_seconds < static_cast<double>(config_.prebuffer_seconds)
                   ? true
                   : flush_startup();
    });
    demuxer_pointer = &demuxer;

    std::clog << "Media stream resolving: " << track.title << '\n';
    const auto stream = extractor_.resolve_stream(track.webpage_url, stop_token);
    if (stream.url.empty() || generation != generation_.load(std::memory_order_acquire) ||
        stop_token.stop_requested()) {
        return;
    }
    std::clog << "Media download started: " << track.title << '\n';
    stream_http(
        stream.url,
        [&](std::span<const std::uint8_t> chunk) { return demuxer.feed(chunk); },
        stream.headers,
        config_.youtube_cookies_file,
        stop_token);
    if (generation != generation_.load(std::memory_order_acquire) ||
        stop_token.stop_requested()) {
        return;
    }
    demuxer.finish();
    if (!startup_complete && !flush_startup()) {
        return;
    }
    std::clog << "Media download completed: " << track.title << '\n';

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
