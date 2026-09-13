// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/drdynvc.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// The DVC server manager ([MS-RDPEDYC] 3.3): a sans-IO multiplexer of
/// dynamic virtual channels over the reassembled messages of the "drdynvc"
/// static virtual channel.
namespace farland::channels {

namespace dvc_event {
/// The capabilities exchange finished; `version` is the negotiated level.
struct CapabilitiesReady {
    std::uint16_t version = 0;
};
/// The client accepted a channel from `DvcServer::open`.
struct ChannelOpened {
    std::uint32_t id = 0;
    std::string name;
};
/// The client refused a channel (no listener, usually). The id is free again.
struct ChannelOpenFailed {
    std::uint32_t id = 0;
    std::string name;
    std::int32_t status = 0;  ///< The negative HRESULT from DYNVC_CREATE_RSP.
};
/// One complete, reassembled and decompressed message from the client.
struct ChannelData {
    std::uint32_t id = 0;
    std::vector<std::byte> data;
};
/// The client closed an open channel. Channels closed with
/// `DvcServer::close` do not produce this event.
struct ChannelClosed {
    std::uint32_t id = 0;
};
}  // namespace dvc_event

using DvcEvent = std::variant<dvc_event::CapabilitiesReady, dvc_event::ChannelOpened, dvc_event::ChannelOpenFailed,
                              dvc_event::ChannelData, dvc_event::ChannelClosed>;

/// Decompresses one RDP_SEGMENTED_DATA block of DYNVC_DATA(_FIRST)_COMPRESSED
/// (RDP 8.0 Lite bulk compression, [MS-RDPEDYC] 2.2.3.3). A decompressor
/// keeps its history between calls; `DvcServer` creates one per channel
/// because each channel has a dedicated decompression context
/// ([MS-RDPEDYC] 3.1.5.2.5).
using DvcDecompressor = std::function<Result<std::vector<std::byte>>(std::span<const std::byte>)>;

/// farland limit on one reassembled client message, per channel.
inline constexpr std::size_t default_max_dvc_message_size = std::size_t{16} * 1024 * 1024;

struct DvcServerConfig {
    /// Highest version offered (1-3). Version 3 is offered only when
    /// `make_decompressor` is set, because a version 3 manager must accept
    /// compressed data ([MS-RDPEDYC] 2.2.3); otherwise at most 2 is offered.
    std::uint16_t max_version = drdynvc::version3;
    drdynvc::PriorityCharges priority_charges = drdynvc::default_priority_charges;
    /// Reassembly limit for channels opened without their own limit.
    std::size_t max_message_size = default_max_dvc_message_size;
    /// Creates the per-channel RDP 8.0 Lite decompressor. Empty: no
    /// compression, and compressed data from the client is a protocol error.
    std::function<DvcDecompressor()> make_decompressor;
};

struct DvcChannelOptions {
    /// Priority class (Pri of DYNVC_CREATE_REQ), 0-3.
    std::uint8_t priority = 0;
    /// Reassembly limit for this channel; defaults to the server's.
    std::optional<std::size_t> max_message_size;
};

/// Server side of [MS-RDPEDYC].
///
/// Input is `receive()` with one reassembled drdynvc message (svc::Reassembler
/// output); output is `take_output()`, a list of drdynvc messages, each at
/// most 1600 bytes, which the caller sends on the drdynvc static channel with
/// `svc::encode_chunks`. Output is unchunked on purpose: the same messages
/// travel without a Channel PDU Header over multitransport tunnels
/// ([MS-RDPEDYC] 1.3.1.2), and each fits one chunk of any valid chunk size.
///
/// Sequencing (decided here where [MS-RDPEDYC] 3.1.5.2.4 leaves it open):
/// - Anything but the Capabilities Response before it, and a second one,
///   are errors.
/// - Create Response, data and Close for an unknown channel id are ignored:
///   the id may belong to a channel closed with `close()` whose client-side
///   traffic is still in flight. Channel ids are allocated monotonically and
///   wrap only after 2^32 - 1 channels, so late PDUs never hit a new channel.
/// - A PDU for a channel whose Create Request was not sent yet, data or Close
///   on a channel still waiting for its Create Response, and a second Create
///   Response are errors.
/// - Compressed data is an error unless version 3 was negotiated and a
///   decompressor is configured.
/// - Soft-Sync Responses are decoded and ignored: farland never sends a
///   Soft-Sync Request, because it has no multitransport.
/// Errors are fatal: `receive()` returns the error now and on every later
/// call, and the caller should end the connection ([MS-RDPEDYC] 3.1.5.2.4).
class DvcServer {
public:
    explicit DvcServer(DvcServerConfig config = {});

    /// Queues the Capabilities Request. Call once, when the drdynvc channel
    /// is joined ([MS-RDPEDYC] 3.3.3.1).
    void start();

    /// Requests a channel to the client listener `name` (ANSI, 1-256
    /// characters, no NUL) and returns its id. The Create Request is queued
    /// now, or after the capabilities exchange if that is still running.
    [[nodiscard]] std::uint32_t open(std::string name, DvcChannelOptions options = {});

    /// Queues one message on an open channel, fragmented into data PDUs of at
    /// most 1600 bytes. Returns false (and sends nothing) unless the channel
    /// is open, e.g. when the client closed it a moment ago.
    [[nodiscard]] bool send(std::uint32_t id, std::span<const std::byte> message);

    /// Closes a channel from the server side ([MS-RDPEDYC] 3.3.5.2). Its id is
    /// forgotten at once, so the client's Close reply and any data still in
    /// flight are ignored. Unknown ids are ignored.
    void close(std::uint32_t id);

    /// Processes one reassembled message from the client.
    [[nodiscard]] Result<void> receive(std::span<const std::byte> message);

    /// Messages to send, in order.
    [[nodiscard]] std::vector<std::vector<std::byte>> take_output();
    [[nodiscard]] std::optional<DvcEvent> poll_event();

    /// The negotiated version once the capabilities exchange is done.
    [[nodiscard]] std::optional<std::uint16_t> version() const noexcept { return version_; }
    /// The version offered by `start()`.
    [[nodiscard]] std::uint16_t offered_version() const noexcept { return offered_; }
    [[nodiscard]] bool is_open(std::uint32_t id) const noexcept;
    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }

private:
    enum class State : std::uint8_t { idle, waiting_caps, ready };
    enum class ChannelState : std::uint8_t { pending, opening, open };

    struct Channel {
        std::string name;
        std::uint8_t priority = 0;
        ChannelState state = ChannelState::pending;
        drdynvc::MessageReassembler reassembler;
        DvcDecompressor decompressor;
    };

    [[nodiscard]] Result<void> handle(const drdynvc::CapsResponse& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::CreateResponse& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::DataFirst& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::Data& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::DataFirstCompressed& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::DataCompressed& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::Close& pdu);
    [[nodiscard]] Result<void> handle(const drdynvc::SoftSyncResponse& pdu);

    [[nodiscard]] Result<void> require_ready() const;
    /// The open channel for a data PDU, nullptr for an unknown id.
    [[nodiscard]] Result<Channel*> data_channel(std::uint32_t id);
    [[nodiscard]] Result<std::vector<std::byte>> decompress(Channel& channel, std::span<const std::byte> data);
    void deliver(std::uint32_t id, std::optional<std::vector<std::byte>> message);
    void send_create(std::uint32_t id, Channel& channel);
    [[nodiscard]] std::uint32_t allocate_id();

    DvcServerConfig config_;
    State state_ = State::idle;
    std::uint16_t offered_ = 0;
    std::optional<std::uint16_t> version_;
    std::uint32_t next_id_ = 1;
    std::map<std::uint32_t, Channel> channels_;
    std::vector<std::uint32_t> pending_;  ///< Opened before the capabilities exchange, in order.
    std::vector<std::vector<std::byte>> output_;
    std::deque<DvcEvent> events_;
    std::optional<Error> error_;
};

}  // namespace farland::channels
