#include "local_audio.hpp"
#include "opus_volume.hpp"
#include "webm_opus.hpp"

#include <opus/opus.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using Bytes = std::vector<std::uint8_t>;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void fails(Function action) {
    try { action(); } catch (const musicbot::LocalAudioError&) { return; }
    throw std::runtime_error("expected LocalAudioError");
}
struct Workspace {
    std::filesystem::path path;
    Workspace() {
        path = std::filesystem::temp_directory_path() /
            ("musicbot-local-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(path), "cannot create test directory");
    }
    ~Workspace() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(static_cast<bool>(output), "cannot write fixture");
}
Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}
Bytes element(Bytes id, const Bytes& payload) {
    require(payload.size() < 127, "fixture element too large");
    id.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
    id.insert(id.end(), payload.begin(), payload.end());
    return id;
}
Bytes silence_packet() {
    int error{};
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
        opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error), opus_encoder_destroy);
    require(error == OPUS_OK && encoder, "cannot create Opus encoder");
    std::vector<opus_int16> pcm(960 * 2);
    Bytes packet(4000);
    const auto size = opus_encode(encoder.get(), pcm.data(), 960, packet.data(), packet.size());
    require(size > 0, "cannot encode Opus packet");
    packet.resize(size);
    return packet;
}
Bytes webm(const Bytes& packet) {
    const auto entry = concat({element({0xD7}, {1}), element({0x83}, {2}),
        element({0x86}, {'A','_','O','P','U','S'}),
        element({0x63,0xA2}, {'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xBB,0,0,0,0,0})});
    return concat({{0x18,0x53,0x80,0x67,0xFF},
        element({0x16,0x54,0xAE,0x6B}, element({0xAE}, entry)),
        element({0x1F,0x43,0xB6,0x75}, concat({
            element({0xA3}, concat({{0x81,0,0,0x80}, packet})),
            element({0xA3}, concat({{0x81,0,20,0x80}, packet}))}))});
}
void test_listing_selection() {
    Workspace work;
    musicbot::LocalAudioLibrary library(work.path / "audio");
    require(library.list().total_files == 0, "missing directory should be empty");
    std::filesystem::create_directory(work.path / "audio");
    for (auto name : {"a.webm", "b.webm", "c.webm", "d.webm", "e.webm", "f.WEBM"})
        write_file(work.path / "audio" / name, {1});
    write_file(work.path / "audio" / "ignored.mp3", {1});
    write_file(work.path / "audio" / ".hidden.webm", {1});
    std::filesystem::create_directory(work.path / "audio" / "directory.webm");
    write_file(work.path / "outside.webm", {1});
#ifndef _WIN32
    std::filesystem::create_symlink(work.path / "outside.webm", work.path / "audio" / "link.webm");
    require(mkfifo((work.path / "audio" / "pipe.webm").c_str(), 0600) == 0, "cannot create FIFO fixture");
#endif
    const auto first = library.list();
    require(first.total_files == 6 && first.total_pages == 2 && first.filenames.size() == 5, "pagination failed");
    require(first.filenames.front() == "a.webm" && library.list(2).filenames.front() == "f.WEBM", "sorting failed");
    fails([&] { static_cast<void>(library.list(0)); });
    fails([&] { static_cast<void>(library.list(3)); });
    for (auto name : {"C:outside.webm", "../outside.webm", "/tmp/outside.webm", "..\\outside.webm", ".env", "link.webm", "pipe.webm", "ignored.mp3", "directory.webm", "missing.webm"})
        fails([&] { library.create_track(name, 1, "user", 2); });
    const std::string unicode = "Моя песня.webm";
    write_file(work.path / "audio" / std::filesystem::u8path(unicode), {1});
    const auto selected = library.create_track(unicode, 12, "listener", 34);
    require(selected.local_filename == unicode && selected.title == "Моя песня", "UTF-8 selection failed");
    require(selected.requester_id == 12 && selected.text_channel_id == 34 && selected.webpage_url.empty(), "metadata incorrect");
    write_file(work.path / "audio" / "0-first.webm", {2});
    require(library.create_track(unicode, 0, "", 0).local_filename == selected.local_filename, "selection changed after catalog edit");
    std::filesystem::remove(work.path / "audio" / std::filesystem::u8path(unicode));
    fails([&] { library.stream(unicode, [](auto) { return true; }); });
}
void test_streaming_and_cancellation() {
    Workspace work;
    musicbot::LocalAudioLibrary library(work.path);
    Bytes contents(200000);
    for (std::size_t i = 0; i < contents.size(); ++i) contents[i] = static_cast<std::uint8_t>(i);
    write_file(work.path / "large.webm", contents);
    Bytes received;
    require(library.stream("large.webm", [&](auto chunk) {
        require(chunk.size() <= 65536, "unbounded read buffer");
        received.insert(received.end(), chunk.begin(), chunk.end()); return true;
    }), "unexpected cancellation");
    require(received == contents, "stream corrupted bytes");
    int callbacks{};
    require(!library.stream("large.webm", [&](auto) { ++callbacks; return false; }), "callback cancellation ignored");
    require(callbacks == 1, "continued after callback cancellation");
    std::stop_source stop;
    callbacks = 0;
    require(!library.stream("large.webm", [&](auto) { ++callbacks; stop.request_stop(); return true; }, stop.get_token()), "stop token ignored");
    require(callbacks == 1, "continued reading after stop");
    require(!library.stream("missing.webm", [](auto) { return true; }, stop.get_token()), "pre-cancelled stream opened a file");
#ifndef _WIN32
    auto track = library.create_track("large.webm", 0, "", 0);
    std::filesystem::remove(work.path / "large.webm");
    write_file(work.path / "other", {1});
    std::filesystem::create_symlink(work.path / "other", work.path / "large.webm");
    fails([&] { library.stream(*track.local_filename, [](auto) { return true; }); });
#endif
}
void test_media_pipeline() {
    Workspace work;
    musicbot::LocalAudioLibrary library(work.path);
    const auto packet = silence_packet();
    write_file(work.path / "silence.webm", webm(packet));
    musicbot::OpusVolume volume(2);
    int packets{};
    musicbot::WebmOpusDemuxer demuxer([&](auto encoded) {
        require(std::equal(packet.begin(), packet.end(), encoded.begin(), encoded.end()), "demux changed packet");
        const auto adjusted = volume.process(encoded, 50);
        require(opus_packet_get_nb_samples(adjusted.data(), adjusted.size(), 48000) == 960, "bad output duration");
        int error{};
        std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
            opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
        require(error == OPUS_OK && decoder, "cannot create decoder");
        std::vector<opus_int16> pcm(1920);
        require(opus_decode(decoder.get(), adjusted.data(), adjusted.size(), pcm.data(), 960, 0) == 960, "output not playable");
        ++packets; return true;
    });
    require(library.stream("silence.webm", [&](auto bytes) { return demuxer.feed(bytes); }), "pipeline cancelled");
    demuxer.finish();
    require(packets == 2, "pipeline lost packets");
    write_file(work.path / "invalid.webm", {0, 1, 2, 3});
    bool rejected{};
    musicbot::WebmOpusDemuxer invalid([](auto) { return true; });
    try {
        library.stream("invalid.webm", [&](auto bytes) { return invalid.feed(bytes); });
        invalid.finish();
    } catch (const musicbot::WebmError&) { rejected = true; }
    require(rejected, "invalid WebM accepted");
}
}  // namespace
int main() {
    try {
        test_listing_selection(); test_streaming_and_cancellation(); test_media_pipeline();
        std::cout << "Local audio tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
