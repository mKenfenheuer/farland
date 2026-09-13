// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/writer.hpp>
#include <farland/channels/dvc_server.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace farland::channels {

DvcServer::DvcServer(DvcServerConfig config) : config_(std::move(config))
{
    FARLAND_ASSERT(config_.max_version >= drdynvc::version1 && config_.max_version <= drdynvc::version3);
}

void DvcServer::start()
{
    FARLAND_ASSERT(state_ == State::idle);
    // [MS-RDPEDYC] 2.2.3: a version 3 manager must accept compressed data.
    offered_ = config_.make_decompressor ? config_.max_version
                                         : std::min<std::uint16_t>(config_.max_version, drdynvc::version2);
    Writer w;
    drdynvc::encode(w, drdynvc::CapsRequest{offered_, config_.priority_charges});
    output_.push_back(std::move(w).take());
    state_ = State::waiting_caps;
}

std::uint32_t DvcServer::open(std::string name, DvcChannelOptions options)
{
    FARLAND_ASSERT(!name.empty() && name.size() <= drdynvc::max_channel_name_size);
    FARLAND_ASSERT(name.find('\0') == std::string::npos);
    FARLAND_ASSERT(options.priority <= 3);
    const std::uint32_t id = allocate_id();
    auto [it, inserted] = channels_.emplace(
        id, Channel{std::move(name),
                    options.priority,
                    ChannelState::pending,
                    drdynvc::MessageReassembler(options.max_message_size.value_or(config_.max_message_size)),
                    {}});
    FARLAND_ASSERT(inserted);
    if (state_ == State::ready && !error_) {
        send_create(id, it->second);
    } else {
        pending_.push_back(id);
    }
    return id;
}

bool DvcServer::send(std::uint32_t id, std::span<const std::byte> message)
{
    const auto it = channels_.find(id);
    if (error_ || it == channels_.end() || it->second.state != ChannelState::open) {
        return false;
    }
    for (auto& pdu : drdynvc::encode_data(id, message)) {
        output_.push_back(std::move(pdu));
    }
    return true;
}

void DvcServer::close(std::uint32_t id)
{
    const auto it = channels_.find(id);
    if (it == channels_.end()) {
        return;
    }
    if (it->second.state == ChannelState::pending) {
        std::erase(pending_, id);
    } else {
        // Also for a channel still waiting for its Create Response: the
        // client processes the Create Request first, so it knows the id.
        Writer w;
        drdynvc::encode(w, drdynvc::Close{id});
        output_.push_back(std::move(w).take());
    }
    channels_.erase(it);
}

Result<void> DvcServer::receive(std::span<const std::byte> message)
{
    if (error_) {
        return std::unexpected(*error_);
    }
    auto result = [&]() -> Result<void> {
        FARLAND_TRY(const auto pdu, drdynvc::decode_client_pdu(message));
        return std::visit([this](const auto& p) { return handle(p); }, pdu);
    }();
    if (!result.has_value()) {
        error_ = result.error();
    }
    return result;
}

std::vector<std::vector<std::byte>> DvcServer::take_output()
{
    return std::exchange(output_, {});
}

std::optional<DvcEvent> DvcServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

bool DvcServer::is_open(std::uint32_t id) const noexcept
{
    const auto it = channels_.find(id);
    return it != channels_.end() && it->second.state == ChannelState::open;
}

// [MS-RDPEDYC] 3.3.3.1.4: adjust to the client's level.
Result<void> DvcServer::handle(const drdynvc::CapsResponse& pdu)
{
    if (state_ == State::idle) {
        return fail(Errc::invalid_value, "DVC Capabilities Response before the request", 0);
    }
    if (state_ == State::ready) {
        return fail(Errc::invalid_value, "second DVC Capabilities Response", 0);
    }
    if (pdu.version == 0) {
        return fail(Errc::invalid_value, "DVC Capabilities Response with version 0", 2);
    }
    version_ = std::min(pdu.version, offered_);
    state_ = State::ready;
    events_.emplace_back(dvc_event::CapabilitiesReady{*version_});
    for (const auto id : std::exchange(pending_, {})) {
        if (const auto it = channels_.find(id); it != channels_.end()) {
            send_create(id, it->second);
        }
    }
    return {};
}

// [MS-RDPEDYC] 3.3.3.2
Result<void> DvcServer::handle(const drdynvc::CreateResponse& pdu)
{
    FARLAND_TRY_VOID(require_ready());
    const auto it = channels_.find(pdu.channel_id);
    if (it == channels_.end()) {
        return {};
    }
    switch (it->second.state) {
    case ChannelState::pending:
        return fail(Errc::invalid_value, "DVC Create Response for a channel not requested yet", 1);
    case ChannelState::open:
        return fail(Errc::invalid_value, "second DVC Create Response", 1);
    case ChannelState::opening:
        break;
    }
    if (pdu.creation_status < 0) {
        events_.emplace_back(
            dvc_event::ChannelOpenFailed{pdu.channel_id, std::move(it->second.name), pdu.creation_status});
        channels_.erase(it);
        return {};
    }
    it->second.state = ChannelState::open;
    events_.emplace_back(dvc_event::ChannelOpened{pdu.channel_id, it->second.name});
    return {};
}

Result<void> DvcServer::handle(const drdynvc::DataFirst& pdu)
{
    FARLAND_TRY(auto* channel, data_channel(pdu.channel_id));
    if (channel != nullptr) {
        FARLAND_TRY(auto message, channel->reassembler.first(pdu.length, pdu.data));
        deliver(pdu.channel_id, std::move(message));
    }
    return {};
}

Result<void> DvcServer::handle(const drdynvc::Data& pdu)
{
    FARLAND_TRY(auto* channel, data_channel(pdu.channel_id));
    if (channel != nullptr) {
        FARLAND_TRY(auto message, channel->reassembler.next(pdu.data));
        deliver(pdu.channel_id, std::move(message));
    }
    return {};
}

// [MS-RDPEDYC] 3.1.5.2.5
Result<void> DvcServer::handle(const drdynvc::DataFirstCompressed& pdu)
{
    FARLAND_TRY(auto* channel, data_channel(pdu.channel_id));
    if (channel != nullptr) {
        FARLAND_TRY(const auto block, decompress(*channel, pdu.data));
        FARLAND_TRY(auto message, channel->reassembler.first(pdu.length, block));
        deliver(pdu.channel_id, std::move(message));
    }
    return {};
}

// [MS-RDPEDYC] 3.1.5.2.6
Result<void> DvcServer::handle(const drdynvc::DataCompressed& pdu)
{
    FARLAND_TRY(auto* channel, data_channel(pdu.channel_id));
    if (channel != nullptr) {
        FARLAND_TRY(const auto block, decompress(*channel, pdu.data));
        FARLAND_TRY(auto message, channel->reassembler.next(block));
        deliver(pdu.channel_id, std::move(message));
    }
    return {};
}

// [MS-RDPEDYC] 3.3.5.2: the server does not answer a client's Close.
Result<void> DvcServer::handle(const drdynvc::Close& pdu)
{
    FARLAND_TRY_VOID(require_ready());
    const auto it = channels_.find(pdu.channel_id);
    if (it == channels_.end()) {
        return {};
    }
    if (it->second.state != ChannelState::open) {
        return fail(Errc::invalid_value, "DVC Close for a channel that is not open", 1);
    }
    channels_.erase(it);
    events_.emplace_back(dvc_event::ChannelClosed{pdu.channel_id});
    return {};
}

Result<void> DvcServer::handle(const drdynvc::SoftSyncResponse& /*pdu*/)
{
    return require_ready();
}

Result<void> DvcServer::require_ready() const
{
    if (state_ != State::ready) {
        return fail(Errc::invalid_value, "drdynvc PDU before the capabilities exchange", 0);
    }
    return {};
}

Result<DvcServer::Channel*> DvcServer::data_channel(std::uint32_t id)
{
    FARLAND_TRY_VOID(require_ready());
    const auto it = channels_.find(id);
    if (it == channels_.end()) {
        return nullptr;
    }
    if (it->second.state != ChannelState::open) {
        return fail(Errc::invalid_value, "DVC data on a channel that is not open", 1);
    }
    return &it->second;
}

Result<std::vector<std::byte>> DvcServer::decompress(Channel& channel, std::span<const std::byte> data)
{
    // [MS-RDPEDYC] 2.2.3.3: compressed PDUs need version 3 on both sides.
    if (version_.value_or(0) < drdynvc::version3) {
        return fail(Errc::invalid_value, "compressed DVC data without version 3", 0);
    }
    if (!channel.decompressor) {
        if (!config_.make_decompressor) {
            return fail(Errc::unsupported, "compressed DVC data without a decompressor", 0);
        }
        channel.decompressor = config_.make_decompressor();
    }
    return channel.decompressor(data);
}

void DvcServer::deliver(std::uint32_t id, std::optional<std::vector<std::byte>> message)
{
    if (message) {
        events_.emplace_back(dvc_event::ChannelData{id, std::move(*message)});
    }
}

void DvcServer::send_create(std::uint32_t id, Channel& channel)
{
    Writer w;
    drdynvc::encode(w, drdynvc::CreateRequest{id, channel.priority, channel.name});
    output_.push_back(std::move(w).take());
    channel.state = ChannelState::opening;
}

std::uint32_t DvcServer::allocate_id()
{
    FARLAND_ASSERT(channels_.size() < std::numeric_limits<std::uint32_t>::max());
    for (;;) {
        const std::uint32_t id = next_id_;
        next_id_ = next_id_ == std::numeric_limits<std::uint32_t>::max() ? 1 : next_id_ + 1;
        if (!channels_.contains(id)) {
            return id;
        }
    }
}

}  // namespace farland::channels
