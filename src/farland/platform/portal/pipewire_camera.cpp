// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/pipewire_camera.hpp>
#include <farland/platform/portal/pipewire_stream.hpp>
#include <farland/platform/portal/pipewire_util.hpp>

#include <pipewire/pipewire.h>
#include <spa/param/format.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

namespace farland::platform::portal {

namespace {

using pw_host::first_data;
using pw_host::StreamHost;

[[nodiscard]] std::uint32_t spa_format_of(VideoFormat format) noexcept
{
    switch (format) {
    case VideoFormat::yuy2:
        return SPA_VIDEO_FORMAT_YUY2;
    case VideoFormat::nv12:
        return SPA_VIDEO_FORMAT_NV12;
    case VideoFormat::i420:
        return SPA_VIDEO_FORMAT_I420;
    case VideoFormat::rgb24:
        // [MS-RDPECAM] RGB24 is B, G, R in memory order, which SPA calls BGR.
        return SPA_VIDEO_FORMAT_BGR;
    case VideoFormat::rgb32:
        return SPA_VIDEO_FORMAT_BGRx;
    }
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

/// Bytes between one row and the next for the formats that have one plane
/// worth speaking of; the planar ones report their luma stride, which is what
/// consumers expect of NV12 and I420 in a single data block.
[[nodiscard]] std::uint32_t row_stride(const VideoMode& mode) noexcept
{
    switch (mode.format) {
    case VideoFormat::yuy2:
        return mode.width * 2;
    case VideoFormat::nv12:
    case VideoFormat::i420:
        return mode.width;
    case VideoFormat::rgb24:
        return mode.width * 3;
    case VideoFormat::rgb32:
        return mode.width * 4;
    }
    return 0;
}

/// An EnumFormat for raw video in `mode`.
const spa_pod* format_pod(pw::PodBuilder& b, const VideoMode& mode)
{
    spa_pod_frame f{};
    b.push_object(&f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    b.prop(SPA_FORMAT_mediaType);
    b.id(SPA_MEDIA_TYPE_video);
    b.prop(SPA_FORMAT_mediaSubtype);
    b.id(SPA_MEDIA_SUBTYPE_raw);
    b.prop(SPA_FORMAT_VIDEO_format);
    b.id(spa_format_of(mode.format));
    b.prop(SPA_FORMAT_VIDEO_size);
    const spa_rectangle size{mode.width, mode.height};
    spa_pod_builder_rectangle(b.get(), size.width, size.height);
    b.prop(SPA_FORMAT_VIDEO_framerate);
    spa_pod_builder_fraction(b.get(), mode.fps, 1);
    return b.pop(&f);
}

/// A Video/Source node whose frames come from the client's camera. The
/// session thread hands whole frames to write(); PipeWire's thread copies the
/// newest one into each buffer it asks for. A frame that is never asked for
/// is simply replaced, so a slow consumer falls behind in time, never in
/// memory.
class VirtualCamera final : public VideoSink {
public:
    VirtualCamera(VideoMode mode, VirtualCameraOptions options)
        : mode_(mode), options_(std::move(options)), frame_bytes_(mode.frame_size())
    {
    }
    VirtualCamera(const VirtualCamera&) = delete;
    VirtualCamera& operator=(const VirtualCamera&) = delete;
    VirtualCamera(VirtualCamera&&) = delete;
    VirtualCamera& operator=(VirtualCamera&&) = delete;
    ~VirtualCamera() override { host_.shutdown(); }

    [[nodiscard]] Result<void> start()
    {
        if (frame_bytes_ == 0 || mode_.width == 0 || mode_.height == 0) {
            return fail(Errc::invalid_value, "the camera has no usable picture size");
        }
        FARLAND_TRY_VOID(host_.connect("farland-camera", -1));
        const std::string rate = std::format("1/{}", std::max(mode_.fps, 1U));
        const std::array<spa_dict_item, 7> items{{
            {PW_KEY_MEDIA_TYPE, "Video"},
            // Video/Source is what a webcam node has; the portal's Camera
            // interface and every browser look for exactly this.
            {PW_KEY_MEDIA_CLASS, "Video/Source"},
            {PW_KEY_MEDIA_ROLE, "Camera"},
            {PW_KEY_APP_NAME, "farland"},
            {PW_KEY_NODE_NAME, options_.node_name.c_str()},
            {PW_KEY_NODE_DESCRIPTION, options_.description.c_str()},
            {PW_KEY_NODE_NICK, options_.description.c_str()},
        }};
        pw::PodBuilder b;
        const std::array<const spa_pod*, 1> params{format_pod(b, mode_)};
        if (b.overflowed()) {
            return fail(Errc::limit_exceeded, "camera format parameters do not fit");
        }
        return host_.start_stream(options_.node_name.c_str(), items, events(), this, PW_DIRECTION_OUTPUT, params);
    }

    void write(std::span<const std::byte> frame) override
    {
        if (frame.size() != frame_bytes_) {
            return;  // not a whole picture in this mode; the caller checked, so this is a guard
        }
        const std::scoped_lock lock(mutex_);
        latest_.assign(frame.begin(), frame.end());
        have_frame_ = true;
    }

    [[nodiscard]] bool closed() const override { return host_.closed(); }
    [[nodiscard]] std::string error() const override { return host_.error(); }

private:
    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state, const char* message)
    {
        static_cast<VirtualCamera*>(data)->host_.state_changed(old, state, message);
    }

    static void on_process(void* data)
    {
        auto& self = *static_cast<VirtualCamera*>(data);
        pw_buffer* buffer = pw_stream_dequeue_buffer(self.host_.stream());
        if (buffer == nullptr) {
            return;
        }
        std::uint32_t size = 0;
        if (spa_data* d = first_data(buffer)) {
            const std::scoped_lock lock(self.mutex_);
            if (self.have_frame_ && d->maxsize >= self.frame_bytes_) {
                std::memcpy(d->data, self.latest_.data(), self.frame_bytes_);
                size = static_cast<std::uint32_t>(self.frame_bytes_);
            }
            d->chunk->offset = 0;
            d->chunk->stride = static_cast<std::int32_t>(row_stride(self.mode_));
            d->chunk->size = size;
        }
        // A buffer with size 0 is a frame the consumer skips, which is what
        // should happen before the client has sent anything.
        pw_stream_queue_buffer(self.host_.stream(), buffer);
    }

    static const pw_stream_events& events()
    {
        static const pw_stream_events e = [] {
            pw_stream_events v{};
            v.version = PW_VERSION_STREAM_EVENTS;
            v.state_changed = &VirtualCamera::on_state_changed;
            v.process = &VirtualCamera::on_process;
            return v;
        }();
        return e;
    }

    VideoMode mode_;
    VirtualCameraOptions options_;
    std::size_t frame_bytes_;
    mutable std::mutex mutex_;
    std::vector<std::byte> latest_;
    bool have_frame_ = false;
    StreamHost host_;  ///< last: shut down (in the destructor) before the frame goes
};

}  // namespace

Result<std::unique_ptr<VideoSink>> create_virtual_camera(const VideoMode& mode, const VirtualCameraOptions& options)
{
    auto camera = std::make_unique<VirtualCamera>(mode, options);
    FARLAND_TRY_VOID(camera->start());
    return camera;
}

}  // namespace farland::platform::portal
