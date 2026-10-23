#include "startup_packet_buffer.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;
const Bytes silence{0xF8, 0xFF, 0xFE}; // One 20 ms Opus packet.
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void test_direct_delivery() {
    musicbot::StartupPacketBuffer buffer(0);
    std::size_t sent{};
    const auto sender = [&](std::span<const std::uint8_t> packet) {
        require(packet.data() == silence.data(), "direct path copied the packet");
        ++sent;
        return true;
    };
    require(buffer.ready(), "zero threshold is not immediately ready");
    require(buffer.feed(silence, sender), "first packet rejected");
    require(sent == 1, "first local packet waits for more input or EOF");
    require(buffer.feed(silence, sender) && sent == 2, "subsequent packet buffered");
    require(buffer.finish(sender) && sent == 2, "finish resent a direct packet");
}

void test_network_prebuffer() {
    musicbot::StartupPacketBuffer buffer(1);
    std::vector<Bytes> received;
    const auto sender = [&](std::span<const std::uint8_t> packet) {
        received.emplace_back(packet.begin(), packet.end()); return true;
    };
    for (int i = 0; i < 49; ++i) {
        auto packet = silence;
        require(buffer.feed(packet, sender), "buffering failed");
        packet.assign(packet.size(), 0); // A demux/encoder buffer can be reused immediately.
        require(received.empty(), "network packets sent before threshold");
    }
    require(buffer.feed(silence, sender), "flush failed");
    require(buffer.ready() && received.size() == 50, "threshold did not flush all packets");
    for (const auto& packet : received) require(packet == silence, "buffered packet corrupted");
    require(buffer.feed(silence, sender) && received.size() == 51, "network resumed buffering after startup");
}

void test_short_stream_and_cancellation() {
    musicbot::StartupPacketBuffer short_stream(60);
    int sent{};
    const auto sender = [&](auto) { ++sent; return true; };
    require(short_stream.feed(silence, sender) && sent == 0, "short stream bypassed threshold");
    require(short_stream.finish(sender) && sent == 1, "short stream lost at EOF");
    require(short_stream.finish(sender) && sent == 1, "short stream duplicated at EOF");

    for (int seconds : {0, 1}) {
        musicbot::StartupPacketBuffer cancelled(seconds);
        int calls{};
        const auto reject = [&](auto) { ++calls; return false; };
        if (seconds == 0) {
            require(!cancelled.feed(silence, reject), "direct cancellation ignored");
        } else {
            for (int i = 0; i < 49; ++i) require(cancelled.feed(silence, reject), "early rejection");
            require(!cancelled.feed(silence, reject), "buffered cancellation ignored");
        }
        require(!cancelled.feed(silence, reject), "continued after cancellation");
        require(!cancelled.finish(reject) && calls == 1, "resent cancelled packets");
    }
}
}  // namespace

int main() {
    try {
        test_direct_delivery(); test_network_prebuffer(); test_short_stream_and_cancellation();
        std::cout << "Startup packet buffer tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
