#include "mcap_reader.h"

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>

#include <nlohmann/json.hpp>

#include <cstring>

namespace mp {

// ── Minimal protobuf wire-format decoder (mirrors EgoViewer's McapReader.cpp;
// no protobuf lib since we only read back messages EgoViewer wrote itself) ──
namespace {

bool pb_read_varint(const uint8_t* buf, size_t size, size_t& pos, uint64_t& value) {
    value = 0;
    int shift = 0;
    while (pos < size) {
        const uint8_t b = buf[pos++];
        value |= (static_cast<uint64_t>(b & 0x7F) << shift);
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

struct PbField {
    int field_number = 0;
    int wire_type = 0;
    uint64_t varint_value = 0;
    uint32_t fixed32_value = 0;
    const uint8_t* bytes = nullptr;
    size_t bytes_len = 0;
};

bool pb_read_field(const uint8_t* buf, size_t size, size_t& pos, PbField& f) {
    if (pos >= size) return false;
    uint64_t tag = 0;
    if (!pb_read_varint(buf, size, pos, tag)) return false;
    f.field_number = static_cast<int>(tag >> 3);
    f.wire_type = static_cast<int>(tag & 0x7);
    switch (f.wire_type) {
        case 0:
            return pb_read_varint(buf, size, pos, f.varint_value);
        case 1:
            if (pos + 8 > size) return false;
            pos += 8;
            return true;
        case 2: {
            uint64_t len = 0;
            if (!pb_read_varint(buf, size, pos, len)) return false;
            if (pos + len > size) return false;
            f.bytes = buf + pos;
            f.bytes_len = len;
            pos += len;
            return true;
        }
        case 5:
            if (pos + 4 > size) return false;
            std::memcpy(&f.fixed32_value, buf + pos, 4);
            pos += 4;
            return true;
        default:
            return false;
    }
}

// google.protobuf.Timestamp{seconds:int64 @1, nanos:int32 @2} -> microseconds.
uint64_t pb_decode_timestamp_us(const uint8_t* buf, size_t size) {
    size_t pos = 0;
    PbField f;
    int64_t sec = 0;
    int32_t nanos = 0;
    while (pb_read_field(buf, size, pos, f)) {
        if (f.field_number == 1) sec = static_cast<int64_t>(f.varint_value);
        else if (f.field_number == 2) nanos = static_cast<int32_t>(f.varint_value);
    }
    return static_cast<uint64_t>(sec) * 1'000'000ULL + static_cast<uint64_t>(nanos) / 1000ULL;
}

std::string pb_str(const PbField& f) {
    return f.bytes ? std::string(reinterpret_cast<const char*>(f.bytes), f.bytes_len) : std::string();
}

} // namespace

bool decode_compressed_video(const std::vector<uint8_t>& payload, DecodedCompressedVideo& out) {
    size_t pos = 0;
    PbField f;
    bool any = false;
    while (pb_read_field(payload.data(), payload.size(), pos, f)) {
        any = true;
        switch (f.field_number) {
            case 1: out.timestamp_us = pb_decode_timestamp_us(f.bytes, f.bytes_len); break;
            case 2: out.frame_id = pb_str(f); break;
            case 3: out.data.assign(f.bytes, f.bytes + f.bytes_len); break;
            case 4: out.format = pb_str(f); break;
            default: break;
        }
    }
    return any;
}

bool decode_raw_audio(const std::vector<uint8_t>& payload, DecodedRawAudio& out) {
    size_t pos = 0;
    PbField f;
    bool any = false;
    while (pb_read_field(payload.data(), payload.size(), pos, f)) {
        any = true;
        switch (f.field_number) {
            case 1: out.timestamp_us = pb_decode_timestamp_us(f.bytes, f.bytes_len); break;
            case 2: out.data.assign(f.bytes, f.bytes + f.bytes_len); break;
            case 3: out.format = pb_str(f); break;
            case 4: out.sample_rate = f.fixed32_value; break;
            case 5: out.channels = f.fixed32_value; break;
            default: break;
        }
    }
    return any;
}

bool decode_imu_sample(const std::vector<uint8_t>& payload, DecodedImuSample& out) {
    try {
        auto j = nlohmann::json::parse(payload.begin(), payload.end());
        if (!j.is_object()) return false;
        out.timestamp_us = j.value("timestamp_us", 0.0);
        out.x = j.value("x", 0.0);
        out.y = j.value("y", 0.0);
        out.z = j.value("z", 0.0);
        return true;
    } catch (...) {
        return false;
    }
}

// ── McapReader ──

struct McapReader::Impl {
    mcap::McapReader reader;
    bool is_open = false;
    uint64_t start_time_us = 0;
    uint64_t end_time_us = 0;
};

McapReader::McapReader() : impl_(std::make_unique<Impl>()) {}
McapReader::~McapReader() { close(); }

bool McapReader::open(const std::string& path) {
    close();
    if (!impl_->reader.open(path).ok()) return false;
    if (!impl_->reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok()) {
        impl_->reader.close();
        return false;
    }
    if (const auto& stats = impl_->reader.statistics()) {
        impl_->start_time_us = stats->messageStartTime / 1000;
        impl_->end_time_us = stats->messageEndTime / 1000;
    }
    impl_->is_open = true;
    return true;
}

void McapReader::close() {
    if (impl_->is_open) {
        impl_->reader.close();
        impl_->is_open = false;
    }
    impl_->start_time_us = 0;
    impl_->end_time_us = 0;
}

bool McapReader::is_open() const { return impl_->is_open; }
uint64_t McapReader::start_time_us() const { return impl_->start_time_us; }
uint64_t McapReader::end_time_us() const { return impl_->end_time_us; }

std::set<std::string> McapReader::topics_with_messages() const {
    std::set<std::string> result;
    if (!impl_->is_open) return result;
    const auto& stats = impl_->reader.statistics();
    if (!stats) return result;
    for (const auto& [channel_id, channel_ptr] : impl_->reader.channels()) {
        if (!channel_ptr) continue;
        auto it = stats->channelMessageCounts.find(channel_id);
        if (it != stats->channelMessageCounts.end() && it->second > 0)
            result.insert(channel_ptr->topic);
    }
    return result;
}

std::map<std::string, uint64_t> McapReader::message_totals() const {
    std::map<std::string, uint64_t> result;
    if (!impl_->is_open) return result;
    const auto& stats = impl_->reader.statistics();
    if (!stats) return result;
    for (const auto& [channel_id, channel_ptr] : impl_->reader.channels()) {
        if (!channel_ptr) continue;
        auto it = stats->channelMessageCounts.find(channel_id);
        if (it != stats->channelMessageCounts.end())
            result[channel_ptr->topic] += it->second;
    }
    return result;
}

uint64_t McapReader::seekable_start_time_us(uint64_t target_us) const {
    if (!impl_->is_open) return 0;
    const mcap::Timestamp target_ns = static_cast<mcap::Timestamp>(target_us) * 1000ULL;
    constexpr mcap::Timestamp kWarmupLeadInNs = 1'000'000'000ULL; // 1s
    mcap::Timestamp best_with_margin = 0, best_any = 0;
    bool found_with_margin = false, found_any = false;
    for (const auto& chunk : impl_->reader.chunkIndexes()) {
        if (chunk.messageStartTime > target_ns) continue;
        if (!found_any || chunk.messageStartTime > best_any) {
            best_any = chunk.messageStartTime;
            found_any = true;
        }
        if (target_ns - chunk.messageStartTime >= kWarmupLeadInNs &&
            (!found_with_margin || chunk.messageStartTime > best_with_margin)) {
            best_with_margin = chunk.messageStartTime;
            found_with_margin = true;
        }
    }
    if (found_with_margin) return static_cast<uint64_t>(best_with_margin / 1000);
    if (found_any) return static_cast<uint64_t>(best_any / 1000);
    return impl_->start_time_us;
}

std::string McapReader::metadata_json(const std::string& name) const {
    if (!impl_->is_open) return {};
    const auto& indexes = impl_->reader.metadataIndexes();
    auto it = indexes.find(name);
    if (it == indexes.end()) return {};
    mcap::Record record;
    if (!mcap::McapReader::ReadRecord(*impl_->reader.dataSource(), it->second.offset, &record).ok())
        return {};
    mcap::Metadata metadata;
    if (!mcap::McapReader::ParseMetadata(record, &metadata).ok()) return {};
    auto json_it = metadata.metadata.find("json");
    return json_it == metadata.metadata.end() ? std::string() : json_it->second;
}

bool McapReader::read_messages(uint64_t start_us, uint64_t end_us, const MessageCallback& cb) {
    if (!impl_->is_open || !cb) return false;
    mcap::ReadMessageOptions options;
    options.startTime = static_cast<mcap::Timestamp>(start_us) * 1000ULL;
    options.endTime = end_us == 0 ? mcap::MaxTime : static_cast<mcap::Timestamp>(end_us) * 1000ULL;
    // Channels are interleaved in write order, not global log-time order —
    // LogTimeOrder keeps the paced playback loop's timestamp deltas positive.
    options.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;

    for (const auto& view : impl_->reader.readMessages([](const mcap::Status&) {}, options)) {
        McapMessage msg;
        msg.topic = view.channel ? view.channel->topic : std::string();
        msg.message_encoding = view.channel ? view.channel->messageEncoding : std::string();
        msg.schema_name = view.schema ? view.schema->name : std::string();
        msg.timestamp_us = view.message.logTime / 1000;
        const auto* bytes = reinterpret_cast<const uint8_t*>(view.message.data);
        msg.data.assign(bytes, bytes + view.message.dataSize);
        if (!cb(msg)) return true;
    }
    return true;
}

} // namespace mp
