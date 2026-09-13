// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/dvc_server.hpp>
#include <farland/channels/svc.hpp>
#include <farland/codec/zgfx.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

/// Dynamic virtual channels over the drdynvc static channel ([MS-RDPEDYC]):
/// reassembles the client's Virtual Channel PDUs into drdynvc messages for
/// the DvcServer, and chunks everything the DvcServer sends. Being the one
/// writer on the drdynvc channel keeps DVC PDUs from interleaving.
namespace farland::server {

class DynamicChannels {
public:
    /// Sends one Virtual Channel PDU (CHANNEL_PDU_HEADER and chunk) on the
    /// drdynvc MCS channel, e.g. through Connection::send_channel_data.
    using SendChunk = std::function<void(std::span<const std::byte> chunk)>;

    /// `chunk_length` is the Virtual Channel chunk size in effect
    /// (svc::negotiated_chunk_length). The default configuration offers
    /// drdynvc version 3 with an RDP 8.0 Lite decompressor per channel.
    DynamicChannels(SendChunk send_chunk, channels::DvcServerConfig config = default_config(),
                    std::size_t chunk_length = channels::svc::chunk_length);

    /// Sends the Capabilities Request. Call once the connection is active.
    void start();
    /// One Virtual Channel PDU from the client (event::ChannelData on the
    /// drdynvc channel). An error ends the connection.
    [[nodiscard]] Result<void> receive(std::span<const std::byte> pdu);

    /// Opens a channel by name; ChannelOpened or ChannelOpenFailed follows.
    [[nodiscard]] std::uint32_t open(std::string name, channels::DvcChannelOptions options = {});
    /// Sends one message on an open channel. False if it is not open.
    bool send(std::uint32_t id, std::span<const std::byte> message);
    void close(std::uint32_t id);

    [[nodiscard]] std::optional<channels::DvcEvent> poll_event() { return dvc_.poll_event(); }
    [[nodiscard]] const channels::DvcServer& server() const noexcept { return dvc_; }

    /// drdynvc version 3, decompressing compressed client data with RDP 8.0
    /// Lite ([MS-RDPEDYC] 3.1.5.2.5), limited to the DvcServer's message size.
    [[nodiscard]] static channels::DvcServerConfig default_config();

private:
    void flush();

    SendChunk send_chunk_;
    std::size_t chunk_length_;
    channels::svc::Reassembler reassembler_;
    channels::DvcServer dvc_;
};

}  // namespace farland::server
