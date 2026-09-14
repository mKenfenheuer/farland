// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/cliprdr_server.hpp>

#include <algorithm>
#include <utility>

namespace farland::channels::cliprdr {

ClipboardServer::ClipboardServer(ServerConfig config) : config_(config) {}

void ClipboardServer::start()
{
    if (state_ != State::idle) {
        return;
    }
    send(Capabilities{caps_version_2, config_.general_flags});
    send(MonitorReady{});
    state_ = State::waiting_caps;
}

void ClipboardServer::send(const Pdu& pdu)
{
    output_.push_back(encode(pdu, has(general_flag::use_long_format_names)));
}

std::vector<std::vector<std::byte>> ClipboardServer::take_output()
{
    return std::exchange(output_, {});
}

std::optional<ServerEvent> ClipboardServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

Result<void> ClipboardServer::receive(std::span<const std::byte> message, Clock::time_point now)
{
    if (error_) {
        return std::unexpected(*error_);
    }
    last_now_ = now;
    if (state_ == State::idle) {
        return {};  // the client speaks after Monitor Ready (1.3.2.1)
    }
    auto pdu = decode(message, has(general_flag::use_long_format_names));
    if (!pdu && pdu.error().code == Errc::unsupported) {
        return {};  // an unknown msgType (3.1.5.1)
    }
    Result<void> handled = pdu ? handle(*pdu, now) : std::unexpected(pdu.error());
    if (!handled) {
        error_ = handled.error();
        incoming_.clear();
        outgoing_.reset();
    }
    return handled;
}

void ClipboardServer::become_ready()
{
    if (state_ == State::ready) {
        return;
    }
    state_ = State::ready;
    events_.emplace_back(server_event::Ready{flags()});
    auto pending = std::exchange(pending_announcement_, std::nullopt);
    if (pending) {
        announce(std::move(*pending));
    }
}

Result<void> ClipboardServer::handle(Pdu& pdu, Clock::time_point now)
{
    if (auto* caps = std::get_if<Capabilities>(&pdu)) {
        // 3.3.5.1.3; a second one changes nothing.
        if (state_ == State::waiting_caps) {
            client_flags_ = caps->general_flags.value_or(0);
            become_ready();
        }
        return {};
    }
    if (std::holds_alternative<TempDirectory>(pdu) || std::holds_alternative<MonitorReady>(pdu)) {
        return {};  // no direct file access (3.1.1.3); Monitor Ready is the server's
    }
    become_ready();  // a client that skips the optional capabilities has the defaults
    if (auto* list = std::get_if<FormatList>(&pdu)) {
        // 3.1.5.2.2: the new formats replace ours on the client.
        send(FormatListResponse{true});
        announced_.clear();
        events_.emplace_back(server_event::RemoteFormatList{std::move(list->formats)});
        return {};
    }
    if (const auto* response = std::get_if<FormatListResponse>(&pdu)) {
        announcement_refused_ = !response->ok;
        events_.emplace_back(server_event::FormatListAnswered{response->ok});
        return {};
    }
    if (const auto* request = std::get_if<FormatDataRequest>(&pdu)) {
        return queue_request(*request, now);
    }
    if (const auto* request = std::get_if<FileContentsRequest>(&pdu)) {
        return queue_request(*request, now);
    }
    if (auto* response = std::get_if<FormatDataResponse>(&pdu)) {
        if (!outgoing_ || outgoing_->file_contents) {
            data_quarantine_until_.reset();  // the late answer to a timed-out request
            return {};
        }
        outgoing_.reset();
        std::optional<std::vector<std::byte>> data;
        if (response->ok) {
            data = std::move(response->data);
        }
        events_.emplace_back(server_event::DataReceived{std::move(data)});
        return {};
    }
    if (auto* response = std::get_if<FileContentsResponse>(&pdu)) {
        return handle_file_contents_response(*response);
    }
    if (const auto* lock = std::get_if<LockClipData>(&pdu)) {
        if (has(general_flag::can_lock_clipdata) && locks_.size() < config_.max_locks &&
            locks_.insert(lock->clip_data_id).second) {
            events_.emplace_back(server_event::Locked{lock->clip_data_id});
        }
        return {};
    }
    if (const auto* unlock = std::get_if<UnlockClipData>(&pdu)) {
        if (locks_.erase(unlock->clip_data_id) != 0) {
            events_.emplace_back(server_event::Unlocked{unlock->clip_data_id});
        }
        return {};  // 3.1.5.3.4: an unknown ID is ignored
    }
    return {};
}

Result<void> ClipboardServer::handle_file_contents_response(FileContentsResponse& response)
{
    if (!outgoing_ || !outgoing_->file_contents || outgoing_->request.stream_id != response.stream_id) {
        return {};  // late, or for a stream nobody asked for
    }
    const auto request = outgoing_->request;
    outgoing_.reset();
    std::optional<std::vector<std::byte>> data;
    if (response.ok) {
        if (request.flags == file_contents::size && response.data.size() != 8) {
            return fail(Errc::invalid_length, "file size response is not 8 bytes", header_size + 4);
        }
        if (response.data.size() > request.requested) {
            return fail(Errc::invalid_length, "file range response longer than requested", header_size + 4);
        }
        data = std::move(response.data);
    }
    events_.emplace_back(server_event::FileContentsReceived{request.stream_id, std::move(data)});
    return {};
}

Result<void> ClipboardServer::queue_request(std::variant<FormatDataRequest, FileContentsRequest> request,
                                            Clock::time_point now)
{
    if (incoming_.size() >= config_.max_queued_requests) {
        return fail(Errc::limit_exceeded, "too many clipboard requests waiting for an answer", 0);
    }
    incoming_.push_back(Incoming{next_request_id_++, request});
    if (!current_handed_out_) {
        next_request(now);
    }
    return {};
}

bool ClipboardServer::allowed(const Incoming& incoming) const
{
    if (announcement_refused_) {
        return false;
    }
    if (const auto* data = std::get_if<FormatDataRequest>(&incoming.request)) {
        return std::ranges::any_of(announced_, [&](const Format& f) { return f.id == data->format_id; });
    }
    const auto& request = std::get<FileContentsRequest>(incoming.request);
    if (!has(general_flag::stream_fileclip_enabled)) {
        return false;
    }
    if (request.clip_data_id && locks_.contains(*request.clip_data_id)) {
        return true;  // the locked file list outlives the announcement
    }
    return std::ranges::any_of(announced_, [](const Format& f) { return f.name == file_group_descriptor_w; });
}

void ClipboardServer::fail_current()
{
    const auto& front = incoming_.front();
    if (const auto* request = std::get_if<FileContentsRequest>(&front.request)) {
        send(FileContentsResponse{false, request->stream_id, {}});
    } else {
        send(FormatDataResponse{false, {}});
    }
    incoming_.pop_front();
    current_handed_out_ = false;
}

void ClipboardServer::next_request(Clock::time_point now)
{
    while (!incoming_.empty() && !current_handed_out_) {
        const auto& front = incoming_.front();
        if (!allowed(front)) {
            fail_current();
            continue;
        }
        current_handed_out_ = true;
        current_since_ = now;
        if (const auto* data = std::get_if<FormatDataRequest>(&front.request)) {
            events_.emplace_back(server_event::DataRequested{front.id, data->format_id});
        } else {
            events_.emplace_back(
                server_event::FileContentsRequested{front.id, std::get<FileContentsRequest>(front.request)});
        }
    }
}

bool ClipboardServer::answer(std::uint64_t id, std::optional<std::span<const std::byte>> data)
{
    if (error_ || !current_handed_out_ || incoming_.empty() || incoming_.front().id != id) {
        return false;
    }
    const auto front = incoming_.front();
    incoming_.pop_front();
    current_handed_out_ = false;
    if (const auto* request = std::get_if<FileContentsRequest>(&front.request)) {
        if (data && request->flags == file_contents::size && data->size() != 8) {
            data.reset();
        }
        FileContentsResponse response{data.has_value(), request->stream_id, {}};
        if (data) {
            const auto bytes = data->first(std::min<std::size_t>(data->size(), request->requested));
            response.data.assign(bytes.begin(), bytes.end());
        }
        send(response);
    } else {
        FormatDataResponse response{data.has_value(), {}};
        if (data) {
            response.data.assign(data->begin(), data->end());
        }
        send(response);
    }
    next_request(last_now_);
    return true;
}

void ClipboardServer::tick(Clock::time_point now)
{
    last_now_ = now;
    if (error_) {
        return;
    }
    if (current_handed_out_ && now - current_since_ >= config_.answer_timeout) {
        fail_current();
        next_request(now);
    }
    if (outgoing_ && now >= outgoing_->deadline) {
        const auto timed_out = *outgoing_;
        outgoing_.reset();
        if (timed_out.file_contents) {
            events_.emplace_back(server_event::FileContentsReceived{timed_out.request.stream_id, std::nullopt});
        } else {
            data_quarantine_until_ = now + config_.request_timeout;
            events_.emplace_back(server_event::DataReceived{std::nullopt});
        }
    }
    if (data_quarantine_until_ && now >= *data_quarantine_until_) {
        data_quarantine_until_.reset();
    }
}

void ClipboardServer::announce(std::vector<Format> formats)
{
    if (error_) {
        return;
    }
    if (state_ != State::ready) {
        pending_announcement_ = std::move(formats);
        return;
    }
    announced_ = formats;
    announcement_refused_ = false;
    send(FormatList{std::move(formats)});
}

bool ClipboardServer::can_request(Clock::time_point now) const
{
    return !error_ && state_ == State::ready && !outgoing_ &&
           (!data_quarantine_until_ || now >= *data_quarantine_until_);
}

bool ClipboardServer::request_data(std::uint32_t format_id, Clock::time_point now)
{
    if (!can_request(now)) {
        return false;
    }
    data_quarantine_until_.reset();
    outgoing_ = Outgoing{false, {}, now + config_.request_timeout};
    send(FormatDataRequest{format_id});
    return true;
}

std::optional<std::uint32_t> ClipboardServer::request_file_contents(std::int32_t index, std::uint32_t flags,
                                                                    std::uint64_t position, std::uint32_t size,
                                                                    std::optional<std::uint32_t> clip_data_id,
                                                                    Clock::time_point now)
{
    if (!can_request(now) || !has(general_flag::stream_fileclip_enabled) ||
        (flags != file_contents::size && flags != file_contents::range) ||
        (position + size > 0xFFFFFFFFU && !has(general_flag::huge_file_support_enabled))) {
        return std::nullopt;
    }
    if (clip_data_id && !has(general_flag::can_lock_clipdata)) {
        clip_data_id.reset();
    }
    FileContentsRequest request{next_stream_id_++, index, flags, 0, 8, clip_data_id};
    if (flags == file_contents::range) {
        request.position = position;
        request.requested = size;
    }
    outgoing_ = Outgoing{true, request, now + config_.request_timeout};
    send(request);
    return request.stream_id;
}

void ClipboardServer::lock(std::uint32_t clip_data_id)
{
    if (!error_ && state_ == State::ready && has(general_flag::can_lock_clipdata)) {
        send(LockClipData{clip_data_id});
    }
}

void ClipboardServer::unlock(std::uint32_t clip_data_id)
{
    if (!error_ && state_ == State::ready && has(general_flag::can_lock_clipdata)) {
        send(UnlockClipData{clip_data_id});
    }
}

}  // namespace farland::channels::cliprdr
