#include "musicbot/webm_opus.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) {
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

Bytes element(Bytes id, const Bytes& payload) {
    require(payload.size() < 127, "test element is too large");
    id.push_back(static_cast<std::uint8_t>(0x80U | payload.size()));
    id.insert(id.end(), payload.begin(), payload.end());
    return id;
}

Bytes opus_head(std::uint8_t channels) {
    return {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, channels,
            0,   0,   0x80, 0xBB, 0,   0,   0,   0,        0};
}

Bytes tracks(std::uint8_t channels = 2) {
    const auto entry = concat({
        element({0xD7}, {1}),
        element({0x83}, {2}),
        element({0x86}, {'A', '_', 'O', 'P', 'U', 'S'}),
        element({0x63, 0xA2}, opus_head(channels)),
    });
    return element({0x16, 0x54, 0xAE, 0x6B}, element({0xAE}, entry));
}

Bytes simple_block(const Bytes& frame, std::uint8_t flags = 0x80) {
    Bytes payload{0x81, 0, 0, flags};
    payload.insert(payload.end(), frame.begin(), frame.end());
    return element({0xA3}, payload);
}

Bytes document(const Bytes& cluster_payload, std::uint8_t channels = 2) {
    Bytes result{0x18, 0x53, 0x80, 0x67, 0xFF};  // Unknown-sized Segment.
    const auto track_data = tracks(channels);
    const auto cluster = element({0x1F, 0x43, 0xB6, 0x75}, cluster_payload);
    result.insert(result.end(), track_data.begin(), track_data.end());
    result.insert(result.end(), cluster.begin(), cluster.end());
    return result;
}

void test_fragmented_stream() {
    std::vector<Bytes> packets;
    musicbot::WebmOpusDemuxer demuxer(
        [&](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
            return true;
        });
    const auto input = document(concat({simple_block({1, 2, 3}), simple_block({4, 5})}));
    for (const auto byte : input) {
        require(demuxer.feed(std::span(&byte, 1)), "demuxer stopped unexpectedly");
    }
    demuxer.finish();
    require(demuxer.opus_track_number() == 1, "wrong Opus track number");
    require(demuxer.opus_channels() == 2, "wrong channel count");
    require(packets == std::vector<Bytes>{{1, 2, 3}, {4, 5}},
            "fragmented packets differ");
    require(demuxer.copied_input_bytes() > 0,
            "fragmented stream unexpectedly required no carry buffer");
}

void test_xiph_lacing() {
    std::vector<Bytes> packets;
    musicbot::WebmOpusDemuxer demuxer(
        [&](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
            return true;
        });
    // Two frames: lace count 1, first size 2, then the frame payloads.
    const auto input = document(simple_block({1, 2, 10, 11, 20, 21, 22}, 0x82));
    demuxer.feed(input);
    demuxer.finish();
    require(packets == std::vector<Bytes>{{10, 11}, {20, 21, 22}}, "Xiph lacing failed");
    require(demuxer.copied_input_bytes() == 0,
            "contiguous stream was copied by the demuxer");
}

void test_fixed_lacing_and_mono_header() {
    std::vector<Bytes> packets;
    musicbot::WebmOpusDemuxer demuxer(
        [&](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
            return true;
        });
    // Two equally-sized frames.
    const auto input = document(simple_block({1, 30, 31, 40, 41}, 0x84), 1);
    demuxer.feed(input);
    demuxer.finish();
    require(demuxer.opus_channels() == 1, "mono OpusHead was ignored");
    require(packets == std::vector<Bytes>{{30, 31}, {40, 41}}, "fixed lacing failed");
}

void test_ebml_lacing() {
    std::vector<Bytes> packets;
    musicbot::WebmOpusDemuxer demuxer(
        [&](std::span<const std::uint8_t> packet) {
            packets.emplace_back(packet.begin(), packet.end());
            return true;
        });
    // Two frames: lace count 1 and first frame size 2 encoded as a VINT.
    const auto input = document(simple_block({1, 0x82, 50, 51, 60, 61, 62}, 0x86));
    demuxer.feed(input);
    demuxer.finish();
    require(packets == std::vector<Bytes>{{50, 51}, {60, 61, 62}}, "EBML lacing failed");
}

void test_callback_cancellation() {
    int calls{};
    musicbot::WebmOpusDemuxer demuxer([&](std::span<const std::uint8_t>) {
        ++calls;
        return false;
    });
    const auto input = document(concat({simple_block({1}), simple_block({2})}));
    require(!demuxer.feed(input), "callback cancellation was ignored");
    require(calls == 1, "packets continued after cancellation");
    demuxer.finish();
}

}  // namespace

int main() {
    try {
        test_fragmented_stream();
        test_xiph_lacing();
        test_fixed_lacing_and_mono_header();
        test_ebml_lacing();
        test_callback_cancellation();
        std::cout << "All WebM/Opus tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
