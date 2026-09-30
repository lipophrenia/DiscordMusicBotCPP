#include "webm_opus.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <utility>

namespace musicbot {
namespace {

constexpr std::uint64_t kSegment = 0x18538067;
constexpr std::uint64_t kTracks = 0x1654AE6B;
constexpr std::uint64_t kCluster = 0x1F43B675;
constexpr std::uint64_t kCues = 0x1C53BB6B;
constexpr std::uint64_t kAttachments = 0x1941A469;
constexpr std::uint64_t kChapters = 0x1043A770;
constexpr std::uint64_t kTags = 0x1254C367;
constexpr std::uint64_t kSeekHead = 0x114D9B74;
constexpr std::uint64_t kInfo = 0x1549A966;
constexpr std::uint64_t kTrackEntry = 0xAE;
constexpr std::uint64_t kBlockGroup = 0xA0;
constexpr std::uint64_t kSimpleBlock = 0xA3;
constexpr std::uint64_t kBlock = 0xA1;
constexpr std::uint64_t kTrackNumber = 0xD7;
constexpr std::uint64_t kTrackType = 0x83;
constexpr std::uint64_t kCodecId = 0x86;
constexpr std::uint64_t kCodecPrivate = 0x63A2;
constexpr std::uint64_t kUnknownEnd = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaximumCapturedLeaf = 4U * 1024U * 1024U;
constexpr std::array<std::uint8_t, 8> kOpusHeadMagic{
    'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};

struct ElementHeader {
    std::uint64_t id{};
    std::uint64_t size{};
    std::size_t header_size{};
    bool unknown_size{};
};

std::size_t vint_length(std::uint8_t first) {
    if (first == 0) {
        throw WebmError("invalid EBML VINT");
    }
    std::size_t length = 1;
    for (std::uint8_t mask = 0x80; (first & mask) == 0; mask >>= 1) {
        ++length;
    }
    return length;
}

std::optional<ElementHeader> read_header(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    const auto id_length = vint_length(bytes[0]);
    if (id_length > 4) {
        throw WebmError("EBML identifier is too long");
    }
    if (bytes.size() < id_length + 1) {
        return std::nullopt;
    }
    std::uint64_t id{};
    for (std::size_t index = 0; index < id_length; ++index) {
        id = (id << 8) | bytes[index];
    }

    const auto size_length = vint_length(bytes[id_length]);
    if (size_length > 8 || bytes.size() < id_length + size_length) {
        return std::nullopt;
    }
    const auto value_bits = static_cast<unsigned>(7 * size_length);
    std::uint64_t size = bytes[id_length] & (0xFFU >> size_length);
    for (std::size_t index = 1; index < size_length; ++index) {
        size = (size << 8) | bytes[id_length + index];
    }
    const auto unknown_value = value_bits == 56
                                   ? ((std::uint64_t{1} << 56) - 1)
                                   : ((std::uint64_t{1} << value_bits) - 1);
    return ElementHeader{id, size, id_length + size_length, size == unknown_value};
}

std::pair<std::uint64_t, std::size_t> read_data_vint(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    if (offset >= bytes.size()) {
        throw WebmError("truncated VINT in WebM block");
    }
    const auto length = vint_length(bytes[offset]);
    if (length > 8 || bytes.size() - offset < length) {
        throw WebmError("truncated VINT in WebM block");
    }
    std::uint64_t value = bytes[offset] & (0xFFU >> length);
    for (std::size_t index = 1; index < length; ++index) {
        value = (value << 8) | bytes[offset + index];
    }
    return {value, length};
}

std::uint64_t unsigned_integer(std::span<const std::uint8_t> payload) {
    if (payload.empty() || payload.size() > 8) {
        throw WebmError("invalid EBML integer");
    }
    std::uint64_t value{};
    for (const auto byte : payload) {
        value = (value << 8) | byte;
    }
    return value;
}

bool is_master(std::uint64_t id) {
    return id == kSegment || id == kTracks || id == kTrackEntry || id == kCluster ||
           id == kBlockGroup;
}

bool is_level_one(std::uint64_t id) {
    return id == kSeekHead || id == kInfo || id == kTracks || id == kCluster ||
           id == kCues || id == kAttachments || id == kChapters || id == kTags;
}

}  // namespace

WebmOpusDemuxer::WebmOpusDemuxer(PacketHandler handler) : handler_(std::move(handler)) {
    if (!handler_) {
        throw WebmError("Opus packet handler is not configured");
    }
}

bool WebmOpusDemuxer::feed(std::span<const std::uint8_t> bytes) {
    if (stopped_) {
        return false;
    }
    std::size_t offset{};
    while (!stopped_) {
        if (pending_) {
            if (pending_->remaining == 0) {
                auto leaf = std::move(*pending_);
                pending_.reset();
                if (leaf.capture) {
                    handle_leaf(leaf.id, split_payload_);
                    split_payload_.clear();
                }
                continue;
            }
            if (offset == bytes.size()) {
                return true;
            }

            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                pending_->remaining, static_cast<std::uint64_t>(bytes.size() - offset)));
            const auto part = bytes.subspan(offset, count);
            if (pending_->capture && split_payload_.empty() &&
                count == pending_->remaining) {
                const auto id = pending_->id;
                offset += count;
                advance(count);
                pending_.reset();
                handle_leaf(id, part);
                continue;
            }
            if (pending_->capture) {
                if (split_payload_.empty()) {
                    split_payload_.reserve(static_cast<std::size_t>(pending_->remaining));
                }
                split_payload_.insert(split_payload_.end(), part.begin(), part.end());
                copied_input_bytes_ += count;
            }
            offset += count;
            advance(count);
            pending_->remaining -= count;
            continue;
        }

        pop_completed_containers();

        if (header_scratch_size_ != 0) {
            bool complete{};
            while (offset < bytes.size()) {
                if (header_scratch_size_ == header_scratch_.size()) {
                    throw WebmError("invalid EBML element header");
                }
                header_scratch_[header_scratch_size_++] = bytes[offset++];
                ++copied_input_bytes_;
                const auto scratch = std::span<const std::uint8_t>(
                    header_scratch_.data(), header_scratch_size_);
                if (const auto header = read_header(scratch)) {
                    if (header->header_size != header_scratch_size_) {
                        throw WebmError("internal EBML header scratch overflow");
                    }
                    begin_element(header->id, header->size, header->header_size,
                                  header->unknown_size);
                    header_scratch_size_ = 0;
                    complete = true;
                    break;
                }
            }
            if (!complete) {
                return true;
            }
            continue;
        }

        if (offset == bytes.size()) {
            return true;
        }
        const auto remaining = bytes.subspan(offset);
        const auto header = read_header(remaining);
        if (!header) {
            if (remaining.size() > header_scratch_.size()) {
                throw WebmError("invalid EBML element header");
            }
            std::copy(remaining.begin(), remaining.end(), header_scratch_.begin());
            header_scratch_size_ = remaining.size();
            copied_input_bytes_ += remaining.size();
            return true;
        }
        offset += header->header_size;
        begin_element(header->id, header->size, header->header_size,
                      header->unknown_size);
    }
    return false;
}

void WebmOpusDemuxer::finish() {
    if (stopped_) {
        return;
    }
    if (pending_ || header_scratch_size_ != 0) {
        throw WebmError("WebM stream ended in the middle of an element");
    }
    while (!containers_.empty() && containers_.back().unknown_size) {
        auto container = std::move(containers_.back());
        containers_.pop_back();
        finish_container(container);
    }
    pop_completed_containers();
    if (!opus_track_number_) {
        throw WebmError("A_OPUS track was not found in WebM");
    }
}

std::optional<std::uint64_t> WebmOpusDemuxer::opus_track_number() const noexcept {
    return opus_track_number_;
}

int WebmOpusDemuxer::opus_channels() const noexcept {
    return opus_channels_;
}

std::uint64_t WebmOpusDemuxer::copied_input_bytes() const noexcept {
    return copied_input_bytes_;
}

void WebmOpusDemuxer::begin_element(
    std::uint64_t id,
    std::uint64_t size,
    std::size_t header_size,
    bool unknown_size) {
    while (!containers_.empty() && containers_.back().id == kCluster &&
           containers_.back().unknown_size && is_level_one(id)) {
        auto cluster = std::move(containers_.back());
        containers_.pop_back();
        finish_container(cluster);
    }

    if (!containers_.empty() && !containers_.back().unknown_size) {
        const auto parent_end = containers_.back().end;
        if (absolute_position_ > parent_end ||
            header_size > parent_end - absolute_position_) {
            throw WebmError("WebM element exceeds its parent boundary");
        }
        const auto payload_start = absolute_position_ + header_size;
        if (!unknown_size && size > parent_end - payload_start) {
            throw WebmError("WebM element exceeds its parent boundary");
        }
    }
    advance(header_size);

    if (is_master(id)) {
        if (unknown_size && id != kSegment && id != kCluster) {
            throw WebmError("unknown size is invalid for this WebM container");
        }
        if (unknown_size && !containers_.empty() &&
            !containers_.back().unknown_size) {
            throw WebmError("unknown-sized container is nested in a finite container");
        }
        if (!unknown_size && size > kUnknownEnd - absolute_position_) {
            throw WebmError("WebM container size overflow");
        }
        Container container;
        container.id = id;
        container.unknown_size = unknown_size;
        container.end = unknown_size ? kUnknownEnd : absolute_position_ + size;
        if (container.id == kTrackEntry) {
            container.track.emplace();
        }
        containers_.push_back(std::move(container));
        return;
    }
    if (unknown_size) {
        throw WebmError("unknown size is only valid for a WebM container");
    }

    const bool in_track_entry = std::ranges::any_of(
        containers_, [](const Container& container) { return container.id == kTrackEntry; });
    const bool capture = id == kSimpleBlock || id == kBlock ||
                         (in_track_entry &&
                          (id == kTrackNumber || id == kTrackType || id == kCodecId ||
                           id == kCodecPrivate));
    if (capture && size > kMaximumCapturedLeaf) {
        throw WebmError("significant WebM element is too large");
    }
    PendingLeaf leaf;
    leaf.id = id;
    leaf.remaining = size;
    leaf.capture = capture;
    pending_ = std::move(leaf);
}

void WebmOpusDemuxer::advance(std::size_t count) {
    if (count > kUnknownEnd - absolute_position_) {
        throw WebmError("WebM stream position overflow");
    }
    absolute_position_ += count;
}

void WebmOpusDemuxer::pop_completed_containers() {
    while (!containers_.empty() && !containers_.back().unknown_size) {
        if (absolute_position_ < containers_.back().end) {
            break;
        }
        if (absolute_position_ > containers_.back().end) {
            throw WebmError("WebM container overflow");
        }
        auto container = std::move(containers_.back());
        containers_.pop_back();
        finish_container(container);
    }
}

void WebmOpusDemuxer::finish_container(Container& container) {
    if (container.id != kTrackEntry || !container.track || opus_track_number_) {
        return;
    }
    const auto& track = *container.track;
    if (track.number && track.type && *track.type == 2 && track.codec_id == "A_OPUS") {
        opus_track_number_ = track.number;
        opus_channels_ = track.channels;
    }
}

void WebmOpusDemuxer::handle_leaf(
    std::uint64_t id,
    std::span<const std::uint8_t> payload) {
    auto track = containers_.rend();
    for (auto iterator = containers_.rbegin(); iterator != containers_.rend(); ++iterator) {
        if (iterator->id == kTrackEntry && iterator->track) {
            track = iterator;
            break;
        }
    }
    if (track != containers_.rend()) {
        if (id == kTrackNumber) {
            track->track->number = unsigned_integer(payload);
            return;
        }
        if (id == kTrackType) {
            track->track->type = unsigned_integer(payload);
            return;
        }
        if (id == kCodecId) {
            track->track->codec_id.assign(payload.begin(), payload.end());
            return;
        }
        if (id == kCodecPrivate && payload.size() >= 10 &&
            std::equal(payload.begin(), payload.begin() + kOpusHeadMagic.size(),
                       kOpusHeadMagic.begin())) {
            const auto channels = static_cast<int>(payload[9]);
            if (channels == 1 || channels == 2) {
                track->track->channels = channels;
            }
            return;
        }
    }
    if (id == kSimpleBlock || id == kBlock) {
        handle_block(payload);
    }
}

bool WebmOpusDemuxer::handle_block(std::span<const std::uint8_t> payload) {
    const auto [track_number, track_length] = read_data_vint(payload, 0);
    if (payload.size() < track_length + 3) {
        throw WebmError("truncated WebM block");
    }
    if (!opus_track_number_ || track_number != *opus_track_number_) {
        return true;
    }
    const auto flags = payload[track_length + 2];
    return emit_laced_frames(payload.subspan(track_length + 3), flags);
}

bool WebmOpusDemuxer::emit_laced_frames(
    std::span<const std::uint8_t> payload,
    std::uint8_t flags) {
    const auto lacing = static_cast<unsigned>((flags >> 1) & 0x03);
    if (lacing == 0) {
        if (payload.empty()) {
            throw WebmError("empty Opus packet");
        }
        if (!handler_(payload)) {
            stopped_ = true;
            return false;
        }
        return true;
    }
    if (payload.empty()) {
        throw WebmError("truncated lacing descriptor");
    }

    const auto frame_count = static_cast<std::size_t>(payload[0]) + 1;
    std::size_t offset = 1;
    std::array<std::size_t, 256> sizes;
    std::size_t size_count{};
    if (lacing == 1) {
        std::size_t declared{};
        for (std::size_t frame = 0; frame + 1 < frame_count; ++frame) {
            std::size_t size{};
            for (;;) {
                if (offset >= payload.size()) {
                    throw WebmError("truncated Xiph lacing");
                }
                const auto value = payload[offset++];
                size += value;
                if (value != 255) {
                    break;
                }
            }
            declared += size;
            sizes[size_count++] = size;
        }
        if (declared > payload.size() - offset) {
            throw WebmError("invalid Xiph lacing");
        }
        sizes[size_count++] = payload.size() - offset - declared;
    } else if (lacing == 2) {
        const auto remaining = payload.size() - offset;
        if (remaining % frame_count != 0) {
            throw WebmError("invalid fixed-size lacing");
        }
        std::fill_n(sizes.begin(), frame_count, remaining / frame_count);
        size_count = frame_count;
    } else {
        const auto [first_size, first_length] = read_data_vint(payload, offset);
        offset += first_length;
        if (first_size > std::numeric_limits<std::size_t>::max()) {
            throw WebmError("EBML-laced frame is too large");
        }
        sizes[size_count++] = static_cast<std::size_t>(first_size);
        std::int64_t declared = static_cast<std::int64_t>(first_size);
        for (std::size_t frame = 1; frame + 1 < frame_count; ++frame) {
            const auto [encoded_delta, length] = read_data_vint(payload, offset);
            offset += length;
            const auto bits = static_cast<unsigned>(7 * length);
            const auto bias = static_cast<std::int64_t>((std::uint64_t{1} << (bits - 1)) - 1);
            const auto delta = static_cast<std::int64_t>(encoded_delta) - bias;
            const auto size = static_cast<std::int64_t>(sizes[size_count - 1]) + delta;
            if (size < 0) {
                throw WebmError("negative EBML-laced frame size");
            }
            sizes[size_count++] = static_cast<std::size_t>(size);
            declared += size;
        }
        if (declared < 0 || static_cast<std::uint64_t>(declared) > payload.size() - offset) {
            throw WebmError("invalid EBML lacing");
        }
        sizes[size_count++] =
            payload.size() - offset - static_cast<std::size_t>(declared);
    }

    for (std::size_t frame = 0; frame < size_count; ++frame) {
        const auto size = sizes[frame];
        if (size == 0 || size > payload.size() - offset) {
            throw WebmError("invalid WebM frame boundary");
        }
        if (!handler_(payload.subspan(offset, size))) {
            stopped_ = true;
            return false;
        }
        offset += size;
    }
    if (offset != payload.size()) {
        throw WebmError("extra data after lacing payload");
    }
    return true;
}

}  // namespace musicbot
