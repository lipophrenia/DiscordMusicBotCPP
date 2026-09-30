#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct OpusDecoder;
struct OpusEncoder;

namespace musicbot {

class OpusVolume {
public:
    explicit OpusVolume(int input_channels);
    ~OpusVolume();

    OpusVolume(const OpusVolume&) = delete;
    OpusVolume& operator=(const OpusVolume&) = delete;

    std::span<const std::uint8_t> process(
        std::span<const std::uint8_t> packet,
        int volume_percent);

private:
    struct DecoderDeleter {
        void operator()(OpusDecoder* value) const noexcept;
    };
    struct EncoderDeleter {
        void operator()(OpusEncoder* value) const noexcept;
    };

    int input_channels_;
    bool passthrough_{true};
    std::unique_ptr<OpusDecoder, DecoderDeleter> decoder_;
    std::unique_ptr<OpusEncoder, EncoderDeleter> encoder_;
    std::vector<std::int16_t> decoded_;
    std::vector<std::int16_t> stereo_;
    std::vector<std::uint8_t> encoded_;
};

}  // namespace musicbot
