#include "startup_packet_buffer.hpp"

#include <opus/opus.h>

#include <algorithm>
#include <stdexcept>

namespace musicbot {

StartupPacketBuffer::StartupPacketBuffer(int seconds) {
    const auto duration = static_cast<std::size_t>(std::max(seconds, 0));
    target_samples_ = static_cast<std::uint64_t>(duration) * 48000U;
    ready_ = duration == 0;
    if (ready_) return;
    packets_.reserve(std::min(duration * 50U + 1U, std::size_t{4096}));
    bytes_.reserve(std::min(duration * 96U * 1024U, std::size_t{2U * 1024U * 1024U}));
}

void StartupPacketBuffer::append(std::span<const std::uint8_t> packet) {
    const auto samples = opus_packet_get_nb_samples(
        packet.data(), static_cast<opus_int32>(packet.size()), 48000);
    if (samples <= 0) throw std::runtime_error("invalid Opus packet duration");
    packets_.push_back({bytes_.size(), packet.size()});
    bytes_.insert(bytes_.end(), packet.begin(), packet.end());
    samples_ += static_cast<std::uint64_t>(samples);
}

}  // namespace musicbot
