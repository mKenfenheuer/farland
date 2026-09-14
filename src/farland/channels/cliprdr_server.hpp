// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/cliprdr.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <span>
#include <variant>
#include <vector>

/// The server endpoint of [MS-RDPECLIP] (3.1, 3.3), sans-IO: capabilities,
/// the copy and paste sequences with delayed rendering in both directions,
/// and file contents. What the desktop's clipboard holds and how formats map
/// is the caller's business.
namespace farland::channels::cliprdr {

struct ServerConfig {
    /// The general capability flags offered; features need both sides.
    std::uint32_t general_flags = general_flag::use_long_format_names | general_flag::stream_fileclip_enabled |
                                  general_flag::fileclip_no_file_paths | general_flag::can_lock_clipdata |
                                  general_flag::huge_file_support_enabled;
    /// How long the client may take to answer request_data() and
    /// request_file_contents().
    std::chrono::milliseconds request_timeout{30'000};
    /// How long the caller may take to answer a client request; then the
    /// client gets CB_RESPONSE_FAIL.
    std::chrono::milliseconds answer_timeout{30'000};
    /// Client requests waiting for their turn. More is a protocol error:
    /// Format Data Responses carry no ID, so they cannot be refused early.
    std::size_t max_queued_requests = 32;
    /// Clip data IDs the client may keep locked at once; further Lock PDUs
    /// are ignored.
    std::size_t max_locks = 64;
};

namespace server_event {
/// The client's capabilities arrived (or its first Format List without
/// them). `flags` holds the general flags both sides set.
struct Ready {
    std::uint32_t flags = 0;
};
/// The client's clipboard changed; the Format List Response went out. The
/// formats announced with announce() are void from now on.
struct RemoteFormatList {
    std::vector<Format> formats;
};
/// The client's answer to announce().
struct FormatListAnswered {
    bool ok = true;
};
/// The client pastes one of the announced formats. Answer with answer().
struct DataRequested {
    std::uint64_t id = 0;
    std::uint32_t format_id = 0;
};
/// The client wants a file's size or a range of it. Answer with answer():
/// 8 bytes (the little-endian size) for a size request, at most
/// `request.requested` bytes for a range.
struct FileContentsRequested {
    std::uint64_t id = 0;
    FileContentsRequest request;
};
/// The answer to request_data(): nullopt when the client failed it or did
/// not answer in time.
struct DataReceived {
    std::optional<std::vector<std::byte>> data;
};
/// The answer to request_file_contents(), as for DataReceived.
struct FileContentsReceived {
    std::uint32_t stream_id = 0;
    std::optional<std::vector<std::byte>> data;
};
/// The client locked the announced clipboard data under `clip_data_id`
/// (CB_CAN_LOCK_CLIPDATA): later File Contents Requests with this ID refer
/// to the file list of now, even after the clipboard changed.
struct Locked {
    std::uint32_t clip_data_id = 0;
};
struct Unlocked {
    std::uint32_t clip_data_id = 0;
};
}  // namespace server_event

using ServerEvent =
    std::variant<server_event::Ready, server_event::RemoteFormatList, server_event::FormatListAnswered,
                 server_event::DataRequested, server_event::FileContentsRequested, server_event::DataReceived,
                 server_event::FileContentsReceived, server_event::Locked, server_event::Unlocked>;

/// Input is receive() with one reassembled "cliprdr" message and tick()
/// with the time; output is take_output(), whole PDUs for svc::encode_chunks,
/// and poll_event().
///
/// Sequencing (decided here where [MS-RDPECLIP] leaves it open):
/// - One request of the server's is outstanding at a time. A request the
///   client does not answer within request_timeout fails. Format Data
///   Responses carry no ID, so after such a timeout request_data() refuses
///   until the late response came or another timeout passed.
/// - Client requests are handed out one at a time, in order, each with an
///   ID that answer() must repeat; one not answered within answer_timeout
///   gets CB_RESPONSE_FAIL, and a late answer() is ignored. Requests for
///   formats that were not announced, or after the client refused the
///   announcement, fail without an event (3.1.5.2.4).
/// - Unknown PDU types and server-to-client PDUs from the client are
///   ignored (3.1.5.1). Responses nobody asked for are ignored too.
/// - Malformed PDUs, a range response longer than requested and a size
///   response other than 8 bytes are fatal: receive() returns the error now
///   and on every later call, and the channel is dead.
class ClipboardServer {
public:
    using Clock = std::chrono::steady_clock;

    explicit ClipboardServer(ServerConfig config = {});

    /// Queues the Clipboard Capabilities and Monitor Ready PDUs (3.3.5.1).
    void start();
    [[nodiscard]] Result<void> receive(std::span<const std::byte> message, Clock::time_point now);
    /// Times out requests.
    void tick(Clock::time_point now);

    /// The desktop's clipboard changed: sends a Format List (at once, or
    /// once the client is ready). Named formats need IDs of their own.
    void announce(std::vector<Format> formats);
    /// Asks for the client's data in `format_id` (an ID from the last
    /// RemoteFormatList). False when a request is outstanding or not allowed.
    [[nodiscard]] bool request_data(std::uint32_t format_id, Clock::time_point now);
    /// Asks for a file's size (file_contents::size) or a range of it; returns
    /// the stream ID, or nullopt as for request_data() and when the client
    /// cannot do stream file transfers. Ranges beyond 4 GiB need
    /// CB_HUGE_FILE_SUPPORT_ENABLED on the client.
    [[nodiscard]] std::optional<std::uint32_t> request_file_contents(std::int32_t index, std::uint32_t flags,
                                                                     std::uint64_t position, std::uint32_t size,
                                                                     std::optional<std::uint32_t> clip_data_id,
                                                                     Clock::time_point now);
    /// Lock and Unlock Clipboard Data (2.2.4), when both sides can.
    void lock(std::uint32_t clip_data_id);
    void unlock(std::uint32_t clip_data_id);
    /// Answers the DataRequested or FileContentsRequested with this ID;
    /// nullopt fails it. Returns false for an ID that is no longer current.
    bool answer(std::uint64_t id, std::optional<std::span<const std::byte>> data);

    [[nodiscard]] std::vector<std::vector<std::byte>> take_output();
    [[nodiscard]] std::optional<ServerEvent> poll_event();

    [[nodiscard]] bool ready() const noexcept { return state_ == State::ready; }
    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }
    /// General flags both sides set; valid once ready.
    [[nodiscard]] std::uint32_t flags() const noexcept { return config_.general_flags & client_flags_; }
    [[nodiscard]] bool has(std::uint32_t flag) const noexcept { return (flags() & flag) == flag; }
    /// A request of the server's is outstanding.
    [[nodiscard]] bool busy() const noexcept { return outgoing_.has_value(); }

private:
    enum class State : std::uint8_t { idle, waiting_caps, ready };

    struct Incoming {
        std::uint64_t id = 0;
        std::variant<FormatDataRequest, FileContentsRequest> request;
    };
    struct Outgoing {
        bool file_contents = false;
        FileContentsRequest request;
        Clock::time_point deadline;
    };

    [[nodiscard]] Result<void> handle(Pdu& pdu, Clock::time_point now);
    [[nodiscard]] Result<void> handle_file_contents_response(FileContentsResponse& response);
    void become_ready();
    [[nodiscard]] Result<void> queue_request(std::variant<FormatDataRequest, FileContentsRequest> request,
                                             Clock::time_point now);
    /// Hands out the next queued request, failing those nobody may answer.
    void next_request(Clock::time_point now);
    [[nodiscard]] bool allowed(const Incoming& incoming) const;
    void fail_current();
    void send(const Pdu& pdu);
    [[nodiscard]] bool can_request(Clock::time_point now) const;

    ServerConfig config_;
    State state_ = State::idle;
    std::uint32_t client_flags_ = 0;
    std::optional<std::vector<Format>> pending_announcement_;
    std::vector<Format> announced_;
    bool announcement_refused_ = false;
    std::deque<Incoming> incoming_;
    bool current_handed_out_ = false;
    Clock::time_point current_since_;
    Clock::time_point last_now_;  ///< from the last receive() or tick()
    std::uint64_t next_request_id_ = 1;
    std::optional<Outgoing> outgoing_;
    std::optional<Clock::time_point> data_quarantine_until_;
    std::uint32_t next_stream_id_ = 1;
    std::set<std::uint32_t> locks_;
    std::vector<std::vector<std::byte>> output_;
    std::deque<ServerEvent> events_;
    std::optional<Error> error_;
};

}  // namespace farland::channels::cliprdr
