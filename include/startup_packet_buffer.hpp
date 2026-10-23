#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace musicbot {

// A zero threshold is a direct path: no packet storage and no startup wait.
class StartupPacketBuffer {
public:
    explicit StartupPacketBuffer(int seconds);

    template <typename Sender>
    bool feed(std::span<const std::uint8_t> packet, Sender&& sender) {
        if (stopped_) return false;
        if (ready_) return deliver(packet, sender);
        append(packet);
        return samples_ < target_samples_ || finish(sender);
    }

    template <typename Sender>
    bool finish(Sender&& sender) {
        if (stopped_) return false;
        for (const auto& packet : packets_) {
            if (!deliver(std::span<const std::uint8_t>(bytes_).subspan(packet.offset, packet.size), sender)) {
                return false;
            }
        }
        packets_.clear();
        bytes_.clear();
        ready_ = true;
        return true;
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] double seconds() const noexcept { return static_cast<double>(samples_) / 48000.0; }

private:
    struct Packet { std::size_t offset; std::size_t size; };
    void append(std::span<const std::uint8_t> packet);

    template <typename Sender>
    bool deliver(std::span<const std::uint8_t> packet, Sender& sender) {
        if (sender(packet)) return true;
        stopped_ = true;
        return false;
    }

    std::vector<Packet> packets_;
    std::vector<std::uint8_t> bytes_;
    std::uint64_t samples_{};
    std::uint64_t target_samples_{};
    bool ready_{};
    bool stopped_{};
};

}  // namespace musicbot
