#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace musicbot {

class WebmError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class WebmOpusDemuxer {
public:
    using PacketHandler = std::function<bool(std::span<const std::uint8_t>)>;

    explicit WebmOpusDemuxer(PacketHandler handler);

    bool feed(std::span<const std::uint8_t> bytes);
    void finish();

    [[nodiscard]] std::optional<std::uint64_t> opus_track_number() const noexcept;
    [[nodiscard]] int opus_channels() const noexcept;
    [[nodiscard]] std::uint64_t copied_input_bytes() const noexcept;

private:
    struct TrackCandidate {
        std::optional<std::uint64_t> number;
        std::optional<std::uint64_t> type;
        std::string codec_id;
        int channels{2};
    };

    struct Container {
        std::uint64_t id{};
        std::uint64_t end{};
        bool unknown_size{};
        std::optional<TrackCandidate> track;
    };

    struct PendingLeaf {
        std::uint64_t id{};
        std::uint64_t remaining{};
        bool capture{};
    };

    void begin_element(
        std::uint64_t id,
        std::uint64_t size,
        std::size_t header_size,
        bool unknown_size);
    void advance(std::size_t count);
    void pop_completed_containers();
    void finish_container(Container& container);
    void handle_leaf(std::uint64_t id, std::span<const std::uint8_t> payload);
    bool handle_block(std::span<const std::uint8_t> payload);
    bool emit_laced_frames(std::span<const std::uint8_t> payload, std::uint8_t flags);

    PacketHandler handler_;
    std::array<std::uint8_t, 12> header_scratch_{};
    std::size_t header_scratch_size_{};
    std::uint64_t absolute_position_{};
    std::vector<Container> containers_;
    std::optional<PendingLeaf> pending_;
    std::vector<std::uint8_t> split_payload_;
    std::optional<std::uint64_t> opus_track_number_;
    int opus_channels_{2};
    std::uint64_t copied_input_bytes_{};
    bool stopped_{};
};

}  // namespace musicbot
