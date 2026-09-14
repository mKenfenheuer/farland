// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/rdpsnd_server.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace farland::channels {

namespace {

constexpr std::uint16_t timestamp16(std::uint32_t ms) noexcept
{
    return static_cast<std::uint16_t>(ms & 0xFFFFU);
}

}  // namespace

RdpsndServer::RdpsndServer(RdpsndServerConfig config) : config_(std::move(config))
{
    FARLAND_ASSERT(!config_.formats.empty() && config_.formats.size() <= rdpsnd::max_formats);
}

void RdpsndServer::start()
{
    FARLAND_ASSERT(state_ == State::idle);
    // [MS-RDPEA] 3.3.5.1.1.1: the first message. cLastBlockConfirmed is
    // arbitrary; the first sample gets the number after it (3.3.5.2.1.1).
    last_block_ = 0xFF;
    queue(rdpsnd::ServerFormats{
        .version = config_.version, .last_block_confirmed = last_block_, .formats = config_.formats});
    state_ = State::waiting_formats;
}

Result<void> RdpsndServer::receive(std::span<const std::byte> message, std::uint32_t now_ms)
{
    if (error_) {
        return std::unexpected(*error_);
    }
    auto pdu = rdpsnd::decode_client_pdu(message);
    Result<void> result;
    if (!pdu) {
        result = std::unexpected(pdu.error());
    } else {
        result = std::visit([this, now_ms](const auto& p) { return handle(p, now_ms); }, *pdu);
    }
    if (!result) {
        error_ = result.error();
    }
    return result;
}

// [MS-RDPEA] 3.3.5.1.1.2 Processing a Client Audio Formats and Version PDU
Result<void> RdpsndServer::handle(const rdpsnd::ClientFormats& pdu, std::uint32_t now_ms)
{
    if (state_ != State::waiting_formats) {
        return fail(Errc::invalid_value, "unexpected Client Audio Formats and Version PDU");
    }
    for (const auto& format : pdu.formats) {
        // 2.2.2.2: each format MUST appear in the server's list.
        if (std::ranges::find(config_.formats, format) == config_.formats.end()) {
            return fail(Errc::invalid_value, "client audio format the server did not offer");
        }
    }
    client_ = pdu;
    if ((pdu.flags & rdpsnd::caps::alive) == 0) {
        // 2.2.3: without TSSNDCAPS_ALIVE there is no data transfer.
        state_ = State::unavailable;
        events_.emplace_back(rdpsnd_event::Unavailable{"the client cannot play audio (no TSSNDCAPS_ALIVE)"});
        return {};
    }
    if (pdu.formats.empty()) {
        state_ = State::unavailable;
        events_.emplace_back(rdpsnd_event::Unavailable{"no audio format in common with the client"});
        return {};
    }
    // 3.3.5.1.1.4: the Training PDU is part of the initialization sequence.
    // UDP is never used (the client's wDGramPort is ignored, 3.3.5.2).
    training_timestamp_ = timestamp16(now_ms);
    training_sent_ms_ = now_ms;
    queue(rdpsnd::Training{.timestamp = training_timestamp_, .data_size = config_.training_data_size});
    state_ = State::training;
    return {};
}

// [MS-RDPEA] 3.3.5.1.1.3 Processing a Quality Mode PDU
Result<void> RdpsndServer::handle(const rdpsnd::QualityMode& pdu, std::uint32_t /*now_ms*/)
{
    if (state_ == State::idle || state_ == State::waiting_formats) {
        return fail(Errc::invalid_value, "Quality Mode PDU before the client's formats");
    }
    quality_mode_ = pdu.mode;
    return {};
}

// [MS-RDPEA] 3.3.5.1.1.5 Processing a Training Confirm PDU
Result<void> RdpsndServer::handle(const rdpsnd::TrainingConfirm& pdu, std::uint32_t now_ms)
{
    if (state_ != State::training) {
        return fail(Errc::invalid_value, "unexpected Training Confirm PDU");
    }
    // A wrong echo makes only the round trip meaningless (2.2.3.2).
    const bool echoed = pdu.timestamp == training_timestamp_;
    state_ = State::ready;
    events_.emplace_back(rdpsnd_event::Ready{
        .client_version = client_.version,
        .client_flags = client_.flags,
        .formats = client_.formats,
        .quality_mode = quality_mode_,
        .training_round_trip_ms = echoed ? now_ms - training_sent_ms_ : 0,
    });
    return {};
}

// [MS-RDPEA] 3.3.5.2.1.6 Processing a Wave Confirm PDU
Result<void> RdpsndServer::handle(const rdpsnd::WaveConfirm& pdu, std::uint32_t /*now_ms*/)
{
    if (state_ != State::ready) {
        return fail(Errc::invalid_value, "Wave Confirm PDU before the data transfer sequence");
    }
    events_.emplace_back(rdpsnd_event::WaveConfirmed{
        .block_no = pdu.block_no,
        .timestamp = pdu.timestamp,
        .sent_timestamp = sent_timestamps_.at(pdu.block_no),
    });
    return {};
}

bool RdpsndServer::uses_wave2() const noexcept
{
    return config_.version >= rdpsnd::version::windows_8 && client_.version >= rdpsnd::version::windows_8;
}

std::uint8_t RdpsndServer::send_wave(std::uint16_t format_no, std::span<const std::byte> data, std::uint32_t now_ms,
                                     std::uint32_t audio_timestamp_ms)
{
    FARLAND_ASSERT(ready() && format_no < client_.formats.size());
    // 3.3.5.2.1.1: one more than the last block, wrapping after 255.
    const auto block = static_cast<std::uint8_t>(last_block_ + 1U);
    last_block_ = block;
    const std::uint16_t timestamp = timestamp16(now_ms);
    sent_timestamps_.at(block) = timestamp;
    if (uses_wave2()) {
        queue(rdpsnd::Wave2{
            .timestamp = timestamp,
            .format_no = format_no,
            .block_no = block,
            .audio_timestamp = audio_timestamp_ms,
            .data = {data.begin(), data.end()},
        });
    } else {
        auto [info, wave] = rdpsnd::encode_wave(
            rdpsnd::Wave{.timestamp = timestamp, .format_no = format_no, .block_no = block, .data = data});
        output_.push_back(std::move(info));
        output_.push_back(std::move(wave));
    }
    return block;
}

void RdpsndServer::send_close()
{
    queue(rdpsnd::Close{});
}

bool RdpsndServer::send_volume(std::uint32_t volume)
{
    // 3.3.5.3.1.1: only for clients with TSSNDCAPS_VOLUME.
    if (!ready() || (client_.flags & rdpsnd::caps::volume) == 0) {
        return false;
    }
    queue(rdpsnd::Volume{volume});
    return true;
}

std::vector<std::vector<std::byte>> RdpsndServer::take_output()
{
    return std::exchange(output_, {});
}

std::optional<RdpsndEvent> RdpsndServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    RdpsndEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void RdpsndServer::queue(const rdpsnd::ServerPdu& pdu)
{
    output_.push_back(rdpsnd::encode_server_pdu(pdu));
}

}  // namespace farland::channels
