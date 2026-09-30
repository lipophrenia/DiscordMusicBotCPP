#include "musicbot/opus_volume.hpp"

#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace musicbot {
namespace {

std::runtime_error make_opus_error(const char* operation, int code) {
    return std::runtime_error(std::string(operation) + ": " + opus_strerror(code));
}

}  // namespace

void OpusVolume::DecoderDeleter::operator()(OpusDecoder* value) const noexcept {
    opus_decoder_destroy(value);
}

void OpusVolume::EncoderDeleter::operator()(OpusEncoder* value) const noexcept {
    opus_encoder_destroy(value);
}

OpusVolume::OpusVolume(int input_channels) : input_channels_(input_channels) {
    if (input_channels_ != 1 && input_channels_ != 2) {
        throw std::invalid_argument("only mono/stereo Opus streams are supported");
    }
    int error{};
    decoder_.reset(opus_decoder_create(48000, input_channels_, &error));
    if (!decoder_ || error != OPUS_OK) {
        throw make_opus_error("failed to create Opus decoder", error);
    }
    encoder_.reset(opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error));
    if (!encoder_ || error != OPUS_OK) {
        throw make_opus_error("failed to create Opus encoder", error);
    }
    opus_encoder_ctl(encoder_.get(), OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(encoder_.get(), OPUS_SET_VBR(1));
    encoded_.resize(4000);
}

OpusVolume::~OpusVolume() = default;

std::span<const std::uint8_t> OpusVolume::process(
    std::span<const std::uint8_t> packet,
    int volume_percent) {
    if (packet.empty()) {
        throw std::invalid_argument("empty Opus packet");
    }
    volume_percent = std::clamp(volume_percent, 0, 200);
    if (volume_percent == 100) {
        passthrough_ = true;
        return packet;
    }
    if (passthrough_) {
        opus_decoder_ctl(decoder_.get(), OPUS_RESET_STATE);
        opus_encoder_ctl(encoder_.get(), OPUS_RESET_STATE);
        passthrough_ = false;
    }

    const auto sample_count = opus_packet_get_nb_samples(
        packet.data(), static_cast<opus_int32>(packet.size()), 48000);
    if (sample_count <= 0 || sample_count > 5760) {
        throw make_opus_error("invalid Opus packet duration", sample_count);
    }
    decoded_.resize(static_cast<std::size_t>(sample_count * input_channels_));
    const auto decoded_samples = opus_decode(
        decoder_.get(), packet.data(), static_cast<opus_int32>(packet.size()), decoded_.data(),
        sample_count, 0);
    if (decoded_samples < 0) {
        throw make_opus_error("Opus decode failed", decoded_samples);
    }

    stereo_.resize(static_cast<std::size_t>(decoded_samples) * 2);
    const auto gain = static_cast<double>(volume_percent) / 100.0;
    for (int sample = 0; sample < decoded_samples; ++sample) {
        for (int channel = 0; channel < 2; ++channel) {
            const auto source_channel = input_channels_ == 1 ? 0 : channel;
            const auto source = decoded_[static_cast<std::size_t>(
                sample * input_channels_ + source_channel)];
            const auto scaled = static_cast<long>(
                std::lround(static_cast<double>(source) * gain));
            stereo_[static_cast<std::size_t>(sample * 2 + channel)] =
                static_cast<std::int16_t>(std::clamp<long>(scaled, -32768, 32767));
        }
    }

    const auto encoded_bytes = opus_encode(
        encoder_.get(), stereo_.data(), decoded_samples, encoded_.data(),
        static_cast<opus_int32>(encoded_.size()));
    if (encoded_bytes < 0) {
        throw make_opus_error("Opus encode failed", encoded_bytes);
    }
    return std::span<const std::uint8_t>(encoded_.data(),
                                         static_cast<std::size_t>(encoded_bytes));
}

}  // namespace musicbot
