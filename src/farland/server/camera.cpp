// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/server/camera.hpp>

#include <algorithm>
#include <format>
#include <tuple>
#include <type_traits>
#include <utility>

namespace farland::server {

namespace {

namespace cam = channels::rdpecam;
namespace dvc = channels::dvc_event;
constexpr std::string_view log_component = "server.camera";

/// How much a media type is wanted, larger being better: an uncompressed
/// picture that fits the limits, as large as it can be, and among equal sizes
/// the frame rate closest to the one asked for.
struct Score {
    std::size_t pixels = 0;
    std::uint32_t rate_distance = 0;
    /// Format preference among equals: planar 4:2:0 costs the least to carry
    /// and is what most consumers want, then 4:2:2, then packed RGB.
    unsigned format_rank = 0;

    friend auto operator<=>(const Score& a, const Score& b) noexcept
    {
        // More pixels first, then the closest frame rate, then the format.
        return std::tuple(a.pixels, b.rate_distance, a.format_rank) <=>
               std::tuple(b.pixels, a.rate_distance, b.format_rank);
    }
};

[[nodiscard]] unsigned format_rank(cam::MediaFormat format) noexcept
{
    switch (format) {
    case cam::MediaFormat::i420:
        return 5;
    case cam::MediaFormat::nv12:
        return 4;
    case cam::MediaFormat::yuy2:
        return 3;
    case cam::MediaFormat::rgb24:
        return 2;
    case cam::MediaFormat::rgb32:
        return 1;
    case cam::MediaFormat::h264:
    case cam::MediaFormat::mjpg:
    case cam::MediaFormat::invalid:
        break;
    }
    return 0;
}

}  // namespace

CameraServer::CameraServer(DynamicChannels& channels, CameraOptions options)
    : channels_(&channels), options_(options), enumerator_id_(channels.open(std::string(cam::enumerator_channel_name)))
{
}

std::optional<cam::MediaType> CameraServer::choose_media_type(std::span<const cam::MediaType> offered,
                                                              const CameraOptions& options)
{
    std::optional<cam::MediaType> best;
    Score best_score;
    for (const auto& type : offered) {
        // Compressed pictures would need a decoder farland does not have.
        if (cam::needs_decoding(type.format) || (type.flags & cam::media_flag::decoding_required) != 0) {
            continue;
        }
        if (type.format == cam::MediaFormat::invalid || type.width == 0 || type.height == 0) {
            continue;
        }
        if (type.width > options.max_width || type.height > options.max_height) {
            continue;
        }
        if (cam::frame_size(type.format, type.width, type.height) == 0) {
            continue;
        }
        const std::uint32_t fps = type.fps();
        const Score score{.pixels = std::size_t{type.width} * type.height,
                          .rate_distance =
                              fps > options.preferred_fps ? fps - options.preferred_fps : options.preferred_fps - fps,
                          .format_rank = format_rank(type.format)};
        if (!best || best_score < score) {
            best = type;
            best_score = score;
        }
    }
    return best;
}

void CameraServer::send_enumerator(const cam::ServerPdu& pdu)
{
    if (!closed_) {
        static_cast<void>(channels_->send(enumerator_id_, cam::encode_server_pdu(pdu, version_)));
    }
}

void CameraServer::send_device(const cam::ServerPdu& pdu)
{
    if (device_id_ && !closed_) {
        static_cast<void>(channels_->send(*device_id_, cam::encode_server_pdu(pdu, version_)));
    }
}

bool CameraServer::handle(const channels::DvcEvent& event)
{
    return std::visit(
        [this](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dvc::CapabilitiesReady>) {
                return false;
            } else {
                const bool is_enumerator = e.id == enumerator_id_;
                const bool is_device = device_id_ && e.id == *device_id_;
                if ((!is_enumerator && !is_device) || closed_) {
                    return false;
                }
                if constexpr (std::is_same_v<T, dvc::ChannelOpened>) {
                    if (is_enumerator) {
                        // [MS-RDPECAM] 3.1.5.1: the client speaks first, with
                        // the version it wants; nothing to do until it does.
                    } else {
                        // [MS-RDPECAM] 3.1.5.2: activate, then ask what it has.
                        state_ = State::streams;
                        send_device(cam::ActivateDeviceRequest{});
                        send_device(cam::StreamListRequest{});
                    }
                } else if constexpr (std::is_same_v<T, dvc::ChannelOpenFailed>) {
                    if (is_enumerator) {
                        close("the client has no camera channel");
                    } else {
                        drop_device("the client refused the camera's channel");
                    }
                } else if constexpr (std::is_same_v<T, dvc::ChannelData>) {
                    const auto handled = is_enumerator ? on_enumerator(e.data) : on_device(e.data);
                    if (!handled) {
                        const auto why = std::format("protocol error: {}", handled.error().message());
                        if (is_enumerator) {
                            close(why);
                        } else {
                            drop_device(why);
                        }
                    }
                } else if constexpr (std::is_same_v<T, dvc::ChannelClosed>) {
                    if (is_enumerator) {
                        closed_ = true;
                        events_.emplace_back(camera_event::Closed{"the client closed the camera channel"});
                    } else {
                        drop_device("the client closed the camera");
                    }
                }
                return true;
            }
        },
        event);
}

Result<void> CameraServer::on_enumerator(std::span<const std::byte> message)
{
    FARLAND_TRY(const auto pdu, cam::decode_client_pdu(message));
    return std::visit(
        [this](const auto& p) -> Result<void> {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, cam::SelectVersionRequest>) {
                // The client asks for a version; the answer is the highest
                // both speak ([MS-RDPECAM] 2.2.2.2).
                if (p.version < cam::version1) {
                    return fail(Errc::invalid_value, "camera client asked for version 0", 0);
                }
                version_ = std::min(p.version, cam::version2);
                state_ = State::listening;
                send_enumerator(cam::SelectVersionResponse{version_});
                log::info(log_component, "camera channel ready, version {}", version_);
            } else if constexpr (std::is_same_v<T, cam::DeviceAdded>) {
                log::info(log_component, "the client offers the camera '{}' on '{}'", p.device_name, p.channel_name);
                if (state_ == State::listening) {
                    open_device(p.device_name, p.channel_name);
                } else {
                    spare_.emplace_back(p.device_name, p.channel_name);
                }
            } else if constexpr (std::is_same_v<T, cam::DeviceRemoved>) {
                std::erase_if(spare_, [&p](const auto& entry) { return entry.second == p.channel_name; });
                if (device_id_ && p.channel_name == device_channel_) {
                    drop_device("the client unplugged the camera");
                }
            } else if constexpr (std::is_same_v<T, cam::ErrorResponse>) {
                return fail(Errc::invalid_value, cam::error_code_name(p.code), 0);
            }
            // A Success Response on the enumerator is an acknowledgement with
            // nothing to do ([MS-RDPECAM] 2.2.3.1).
            return {};
        },
        pdu);
}

void CameraServer::open_device(const std::string& device_name, const std::string& channel_name)
{
    device_name_ = device_name;
    device_channel_ = channel_name;
    state_ = State::opening;
    sample_pending_ = false;
    device_id_ = channels_->open(channel_name);
}

Result<void> CameraServer::on_device(std::span<const std::byte> message)
{
    FARLAND_TRY(const auto pdu, cam::decode_client_pdu(message));
    return std::visit(
        [this](const auto& p) -> Result<void> {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, cam::StreamListResponse>) {
                if (state_ != State::streams) {
                    return {};  // out of sequence: ignore ([MS-RDPECAM] 3.1.5)
                }
                // The first capture stream the camera marked selected, or
                // else the first capture stream at all.
                std::optional<std::uint8_t> chosen;
                for (std::size_t i = 0; i < p.streams.size(); ++i) {
                    if (p.streams[i].category != cam::stream_category_capture) {
                        continue;
                    }
                    if (!chosen || p.streams[i].selected) {
                        chosen = static_cast<std::uint8_t>(i);
                    }
                    if (p.streams[i].selected) {
                        break;
                    }
                }
                if (!chosen) {
                    drop_device("the camera has no capture stream");
                    return {};
                }
                stream_index_ = *chosen;
                state_ = State::media_types;
                send_device(cam::MediaTypeListRequest{stream_index_});
            } else if constexpr (std::is_same_v<T, cam::MediaTypeListResponse>) {
                if (state_ != State::media_types) {
                    return {};
                }
                const auto chosen = choose_media_type(p.media_types, options_);
                if (!chosen) {
                    drop_device(std::format(
                        "the camera offers no format farland can read ({} media types, all compressed or too large)",
                        p.media_types.size()));
                    return {};
                }
                // The description goes back exactly as the client sent it:
                // its flags are the client's, which its own validation accepts.
                media_type_ = *chosen;
                state_ = State::starting;
                log::info(log_component, "camera '{}': {} {}x{} at {} fps", device_name_,
                          cam::format_name(media_type_.format), media_type_.width, media_type_.height,
                          media_type_.fps());
                send_device(cam::StartStreamsRequest{stream_index_, media_type_});
            } else if constexpr (std::is_same_v<T, cam::SuccessResponse>) {
                if (state_ == State::starting) {
                    state_ = State::streaming;
                    events_.emplace_back(camera_event::Opened{device_name_, media_type_});
                    request_frame();
                }
            } else if constexpr (std::is_same_v<T, cam::SampleResponse>) {
                if (state_ != State::streaming || p.stream_index != stream_index_) {
                    return {};
                }
                sample_pending_ = false;
                const std::size_t wanted = cam::frame_size(media_type_.format, media_type_.width, media_type_.height);
                if (p.sample.size() != wanted) {
                    // A short frame would be read past its end by any
                    // consumer; a long one means the client and the server
                    // disagree about the format.
                    drop_device(std::format("the camera sent a {}-byte frame where {} bytes are a {} {}x{} picture",
                                            p.sample.size(), wanted, cam::format_name(media_type_.format),
                                            media_type_.width, media_type_.height));
                    return {};
                }
                events_.emplace_back(camera_event::Frame{{p.sample.begin(), p.sample.end()}});
            } else if constexpr (std::is_same_v<T, cam::SampleErrorResponse>) {
                drop_device(std::format("the camera failed: {}", cam::error_code_name(p.code)));
            } else if constexpr (std::is_same_v<T, cam::ErrorResponse>) {
                drop_device(std::format("the camera refused the request: {}", cam::error_code_name(p.code)));
            }
            // Current Media Type, Property List and Property Value responses
            // are never asked for, and anything else out of sequence is
            // ignored ([MS-RDPECAM] 3.1.5).
            return {};
        },
        pdu);
}

void CameraServer::request_frame()
{
    if (state_ != State::streaming || sample_pending_) {
        return;
    }
    sample_pending_ = true;
    send_device(cam::SampleRequest{stream_index_});
}

void CameraServer::stop()
{
    if (!device_id_) {
        return;
    }
    if (state_ == State::streaming || state_ == State::starting) {
        send_device(cam::StopStreamsRequest{});
    }
    send_device(cam::DeactivateDeviceRequest{});
    channels_->close(*device_id_);
    device_id_.reset();
    sample_pending_ = false;
    state_ = closed_ ? state_ : State::listening;
}

void CameraServer::drop_device(std::string reason)
{
    const bool was_streaming = state_ == State::streaming;
    if (device_id_) {
        channels_->close(*device_id_);
        device_id_.reset();
    }
    sample_pending_ = false;
    device_name_.clear();
    device_channel_.clear();
    state_ = State::listening;
    if (was_streaming) {
        log::info(log_component, "camera gone: {}", reason);
    } else {
        log::warn(log_component, "camera not used: {}", reason);
    }
    events_.emplace_back(camera_event::Closed{std::move(reason)});
    // Another camera may have been offered while this one was in use.
    if (!spare_.empty()) {
        const auto next = spare_.front();
        spare_.erase(spare_.begin());
        open_device(next.first, next.second);
    }
}

void CameraServer::close(std::string reason)
{
    if (closed_) {
        return;
    }
    log::warn(log_component, "camera channel closed: {}", reason);
    closed_ = true;
    if (device_id_) {
        channels_->close(*device_id_);
        device_id_.reset();
    }
    channels_->close(enumerator_id_);
    events_.emplace_back(camera_event::Closed{std::move(reason)});
}

std::optional<CameraEvent> CameraServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    CameraEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

}  // namespace farland::server
