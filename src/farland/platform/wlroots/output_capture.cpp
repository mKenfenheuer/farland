// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/wlroots/output_capture.hpp>
#include <farland/platform/wlroots/wayland/connection.hpp>
#include <farland/platform/wlroots/wayland/shm.hpp>

#include <algorithm>
#include <array>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <functional>
#include <optional>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr-screencopy-unstable-v1-client-protocol.h>

namespace farland::platform::wlroots {

namespace {

namespace shm_format = wayland::shm_format;
using wayland::ShmBuffer;
constexpr std::string_view log_component = "platform.wlroots.capture";
/// Captures that fail one after another before the capture gives up.
constexpr int max_failures = 5;
/// A frame the consumer holds, one waiting and one being captured.
constexpr std::size_t max_buffers = 3;

/// An eventfd that is readable while something is pending.
class WakeFd {
public:
    WakeFd() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
    [[nodiscard]] int get() const noexcept { return fd_.get(); }
    void signal() const noexcept
    {
        const std::uint64_t one = 1;
        if (::write(fd_.get(), &one, sizeof(one)) < 0) {
            // Full (EAGAIN): readable anyway.
        }
    }
    void clear() const noexcept
    {
        std::uint64_t count = 0;
        if (::read(fd_.get(), &count, sizeof(count)) < 0) {
            // Empty (EAGAIN): nothing was pending.
        }
    }

private:
    UniqueFd fd_;
};

/// A capture session's buffer constraints, as a batch ending in done.
struct Constraints {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> shm_formats;

    /// XRGB8888, else ARGB8888; nullopt when the session offers neither.
    [[nodiscard]] std::optional<std::uint32_t> format() const
    {
        for (const auto wanted : {shm_format::xrgb8888, shm_format::argb8888}) {
            if (std::ranges::find(shm_formats, wanted) != shm_formats.end()) {
                return wanted;
            }
        }
        return std::nullopt;
    }
};

/// Listens to an ext_image_copy_capture_session_v1.
struct SessionListener {
    Constraints pending;
    std::optional<Constraints> current;
    std::function<void()> on_done;
    std::function<void()> on_stopped;
};

/// Its listener, once SessionListener is complete.
constexpr ext_image_copy_capture_session_v1_listener session_listener{
    .buffer_size =
        [](void* data, ext_image_copy_capture_session_v1* /*session*/, std::uint32_t width, std::uint32_t height) {
            auto* self = static_cast<SessionListener*>(data);
            self->pending.width = width;
            self->pending.height = height;
        },
    .shm_format =
        [](void* data, ext_image_copy_capture_session_v1* /*session*/, std::uint32_t format) {
            static_cast<SessionListener*>(data)->pending.shm_formats.push_back(format);
        },
    .dmabuf_device = [](void* /*data*/, ext_image_copy_capture_session_v1* /*session*/, wl_array* /*device*/) {},
    .dmabuf_format = [](void* /*data*/, ext_image_copy_capture_session_v1* /*session*/, std::uint32_t /*format*/,
                        wl_array* /*modifiers*/) {},
    .done =
        [](void* data, ext_image_copy_capture_session_v1* /*session*/) {
            auto* self = static_cast<SessionListener*>(data);
            self->current = std::exchange(self->pending, Constraints{});
            self->on_done();
        },
    .stopped =
        [](void* data, ext_image_copy_capture_session_v1* /*session*/) {
            static_cast<SessionListener*>(data)->on_stopped();
        },
};

void flip_rows(std::span<std::byte> pixels, std::size_t stride, std::uint32_t height)
{
    for (std::uint32_t y = 0; y < height / 2; ++y) {
        auto top = pixels.subspan(y * stride, stride);
        auto bottom = pixels.subspan((height - 1 - y) * stride, stride);
        std::ranges::swap_ranges(top, bottom);
    }
}

}  // namespace

void Damage::add(const Rect& rect)
{
    if (all_ || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (rects_.size() < max_rects) {
        rects_.push_back(rect);
        return;
    }
    std::int32_t left = rect.x;
    std::int32_t top = rect.y;
    std::int32_t right = rect.x + rect.width;
    std::int32_t bottom = rect.y + rect.height;
    for (const auto& r : rects_) {
        left = std::min(left, r.x);
        top = std::min(top, r.y);
        right = std::max(right, r.x + r.width);
        bottom = std::max(bottom, r.y + r.height);
    }
    rects_.assign(1, Rect{left, top, right - left, bottom - top});
}

void Damage::merge(const Damage& other)
{
    if (other.all_) {
        add_all();
        rects_.clear();
        return;
    }
    for (const auto& r : other.rects_) {
        add(r);
    }
}

void unpremultiply(std::span<std::byte> bgra) noexcept
{
    for (std::size_t i = 0; i + 4 <= bgra.size(); i += 4) {
        auto pixel = bgra.subspan(i, 4);
        const auto alpha = std::to_integer<unsigned>(pixel[3]);
        if (alpha == 255) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const unsigned value =
                alpha == 0 ? 0U : std::min(255U, ((std::to_integer<unsigned>(pixel[c]) * 255U) + (alpha / 2)) / alpha);
            pixel[c] = static_cast<std::byte>(value);
        }
    }
}

struct OutputCapture::Impl {
    struct Buffer {
        enum class State : std::uint8_t { free, capturing, pending, held };
        std::unique_ptr<ShmBuffer> shm;
        /// Where the screen changed since this buffer was last filled.
        Damage stale;
        bool fresh = true;
        State state = State::free;
    };

    class Frames final : public FrameSource {
    public:
        explicit Frames(Impl* impl) : impl_(impl) {}
        [[nodiscard]] int wake_fd() const noexcept override { return impl_->frame_wake.get(); }
        [[nodiscard]] std::optional<Frame> take_frame() override { return impl_->take_frame(); }
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> size() const override { return impl_->size; }
        void release_frame() override { impl_->release_frame(); }

    private:
        Impl* impl_;
    };

    /// The pointer cursor of the output, from an ext cursor session.
    class Cursor final : public CursorSource {
    public:
        Cursor(Impl* impl, ext_image_copy_capture_cursor_session_v1* cursor_session)
            : impl_(impl), cursor_session_(cursor_session),
              session_(ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor_session))
        {
            static constexpr ext_image_copy_capture_cursor_session_v1_listener listener{
                .enter =
                    [](void* data, ext_image_copy_capture_cursor_session_v1* /*session*/) {
                        static_cast<Cursor*>(data)->show(true);
                    },
                .leave =
                    [](void* data, ext_image_copy_capture_cursor_session_v1* /*session*/) {
                        static_cast<Cursor*>(data)->show(false);
                    },
                .position =
                    [](void* data, ext_image_copy_capture_cursor_session_v1* /*session*/, std::int32_t x,
                       std::int32_t y) {
                        auto* self = static_cast<Cursor*>(data);
                        self->update_.position = std::pair{x, y};
                        self->changed();
                    },
                .hotspot = [](void* data, ext_image_copy_capture_cursor_session_v1* /*session*/, std::int32_t x,
                              std::int32_t y) { static_cast<Cursor*>(data)->next_hotspot_ = std::pair{x, y}; },
            };
            ext_image_copy_capture_cursor_session_v1_add_listener(cursor_session_, &listener, this);
            constraints_.on_done = [this] { capture(); };
            constraints_.on_stopped = [this] {
                log::warn(log_component, "{}: the cursor capture stopped", impl_->name);
                show(false);
            };
            ext_image_copy_capture_session_v1_add_listener(session_, &session_listener, &constraints_);
        }
        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;
        Cursor(Cursor&&) = delete;
        Cursor& operator=(Cursor&&) = delete;
        ~Cursor() override
        {
            if (frame_ != nullptr) {
                ext_image_copy_capture_frame_v1_destroy(frame_);
            }
            buffer_.reset();
            ext_image_copy_capture_session_v1_destroy(session_);
            ext_image_copy_capture_cursor_session_v1_destroy(cursor_session_);
        }

        [[nodiscard]] int wake_fd() const noexcept override { return wake_.get(); }
        [[nodiscard]] std::optional<CursorUpdate> take_cursor() override
        {
            wake_.clear();
            if (!has_update_) {
                return std::nullopt;
            }
            has_update_ = false;
            auto update = std::exchange(update_, CursorUpdate{});
            update.visible = visible_;
            return update;
        }

    private:
        void changed()
        {
            has_update_ = true;
            wake_.signal();
        }
        void show(bool visible)
        {
            visible_ = visible;
            changed();
        }

        void capture()
        {
            if (frame_ != nullptr || !constraints_.current) {
                return;
            }
            const auto& c = *constraints_.current;
            const auto format = c.format();
            if (!format || c.width == 0 || c.height == 0) {
                return;
            }
            if (!buffer_ || buffer_->width() != c.width || buffer_->height() != c.height ||
                buffer_->format() != *format) {
                auto buffer = ShmBuffer::create(impl_->protocols.shm, c.width, c.height, c.width * 4, *format);
                if (!buffer) {
                    return;
                }
                buffer_ = std::move(*buffer);
            }
            frame_ = ext_image_copy_capture_session_v1_create_frame(session_);
            static constexpr ext_image_copy_capture_frame_v1_listener listener{
                .transform = [](void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/,
                                std::uint32_t /*transform*/) {},
                .damage = [](void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/, std::int32_t /*x*/,
                             std::int32_t /*y*/, std::int32_t /*width*/, std::int32_t /*height*/) {},
                .presentation_time = [](void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/,
                                        std::uint32_t /*hi*/, std::uint32_t /*lo*/, std::uint32_t /*nsec*/) {},
                .ready = [](void* data,
                            ext_image_copy_capture_frame_v1* /*frame*/) { static_cast<Cursor*>(data)->ready(); },
                .failed =
                    [](void* data, ext_image_copy_capture_frame_v1* /*frame*/, std::uint32_t reason) {
                        auto* self = static_cast<Cursor*>(data);
                        ext_image_copy_capture_frame_v1_destroy(self->frame_);
                        self->frame_ = nullptr;
                        // Buffer constraints: wait for the new ones; stopped: done.
                        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN &&
                            ++self->failures_ < max_failures) {
                            self->capture();
                        }
                    },
            };
            ext_image_copy_capture_frame_v1_add_listener(frame_, &listener, this);
            ext_image_copy_capture_frame_v1_attach_buffer(frame_, buffer_->buffer());
            ext_image_copy_capture_frame_v1_damage_buffer(frame_, 0, 0, static_cast<std::int32_t>(c.width),
                                                          static_cast<std::int32_t>(c.height));
            ext_image_copy_capture_frame_v1_capture(frame_);
        }

        void ready()
        {
            ext_image_copy_capture_frame_v1_destroy(frame_);
            frame_ = nullptr;
            failures_ = 0;
            hotspot_ = next_hotspot_;
            update_.shape = crop();
            changed();
            capture();
        }

        /// The cursor image without its transparent border, straight alpha.
        [[nodiscard]] CursorImage crop() const
        {
            const auto width = buffer_->width();
            const auto height = buffer_->height();
            const auto pixels = std::span<const std::byte>(buffer_->data());
            std::uint32_t left = width;
            std::uint32_t top = height;
            std::uint32_t right = 0;
            std::uint32_t bottom = 0;
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    if (pixels[(std::size_t{y} * width * 4) + (std::size_t{x} * 4) + 3] != std::byte{0}) {
                        left = std::min(left, x);
                        top = std::min(top, y);
                        right = std::max(right, x + 1);
                        bottom = std::max(bottom, y + 1);
                    }
                }
            }
            CursorImage image;
            if (right <= left || bottom <= top) {
                // Fully transparent: one clear pixel.
                image.width = 1;
                image.height = 1;
                image.pixels.assign(4, std::byte{0});
                return image;
            }
            image.width = right - left;
            image.height = bottom - top;
            image.hotspot_x = hotspot_.first - static_cast<std::int32_t>(left);
            image.hotspot_y = hotspot_.second - static_cast<std::int32_t>(top);
            image.pixels.reserve(std::size_t{image.width} * image.height * 4);
            for (std::uint32_t y = top; y < bottom; ++y) {
                const auto row = pixels.subspan((std::size_t{y} * width * 4) + (std::size_t{left} * 4),
                                                std::size_t{image.width} * 4);
                image.pixels.insert(image.pixels.end(), row.begin(), row.end());
            }
            unpremultiply(image.pixels);
            return image;
        }

        Impl* impl_;
        ext_image_copy_capture_cursor_session_v1* cursor_session_;
        ext_image_copy_capture_session_v1* session_;
        ext_image_copy_capture_frame_v1* frame_ = nullptr;
        SessionListener constraints_;
        std::unique_ptr<ShmBuffer> buffer_;
        std::pair<std::int32_t, std::int32_t> hotspot_{0, 0};
        std::pair<std::int32_t, std::int32_t> next_hotspot_{0, 0};
        CursorUpdate update_;
        bool has_update_ = false;
        bool visible_ = false;
        int failures_ = 0;
        WakeFd wake_;
    };

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl()
    {
        cursor.reset();
        if (ext_frame != nullptr) {
            ext_image_copy_capture_frame_v1_destroy(ext_frame);
        }
        if (session != nullptr) {
            ext_image_copy_capture_session_v1_destroy(session);
        }
        if (source != nullptr) {
            ext_image_capture_source_v1_destroy(source);
        }
        if (screencopy_frame != nullptr) {
            zwlr_screencopy_frame_v1_destroy(screencopy_frame);
        }
        buffers.clear();
        connection->flush();
    }

    wayland::Connection* connection = nullptr;
    wl_output* output = nullptr;
    Protocols protocols;
    std::string name;
    bool closed = false;
    std::string error;
    WakeFd frame_wake;
    Frames frames{this};

    std::vector<std::unique_ptr<Buffer>> buffers;
    Buffer* capturing = nullptr;
    Buffer* pending = nullptr;
    Buffer* held = nullptr;
    /// What changed in the frame being captured.
    Damage frame_damage;
    /// What changed between the frame taken last and the pending one.
    Damage pending_damage;
    /// The next frame counts as changed everywhere (the first, a new size).
    bool force_full = true;
    std::pair<std::uint32_t, std::uint32_t> size{0, 0};
    std::uint64_t sequence = 0;
    int failures = 0;

    // ext-image-copy-capture-v1
    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* ext_frame = nullptr;
    SessionListener constraints;
    std::unique_ptr<Cursor> cursor;

    // wlr-screencopy-unstable-v1
    zwlr_screencopy_frame_v1* screencopy_frame = nullptr;
    struct ScreencopyBuffer {
        std::uint32_t format = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t stride = 0;
    };
    std::optional<ScreencopyBuffer> screencopy_buffer;
    bool y_invert = false;
    /// The copy in progress takes the whole frame (copy, not copy_with_damage).
    bool screencopy_whole = false;

    void fail(std::string_view why)
    {
        if (closed) {
            return;
        }
        closed = true;
        error = why;
        log::warn(log_component, "{}: {}", name, why);
        // Readable for good, like a socket at end of file.
        frame_wake.signal();
    }

    /// A free buffer of this layout, made when there are fewer than three.
    Buffer* free_buffer(std::uint32_t width, std::uint32_t height, std::uint32_t stride, std::uint32_t format)
    {
        std::erase_if(buffers, [&](const std::unique_ptr<Buffer>& b) {
            return b->state == Buffer::State::free && (b->shm->width() != width || b->shm->height() != height ||
                                                       b->shm->stride() != stride || b->shm->format() != format);
        });
        for (auto& b : buffers) {
            if (b->state == Buffer::State::free) {
                return b.get();
            }
        }
        if (buffers.size() >= max_buffers) {
            return nullptr;
        }
        auto shm = ShmBuffer::create(protocols.shm, width, height, stride, format);
        if (!shm) {
            fail("cannot allocate a capture buffer");
            return nullptr;
        }
        auto buffer = std::make_unique<Buffer>();
        buffer->shm = std::move(*shm);
        buffers.push_back(std::move(buffer));
        return buffers.back().get();
    }

    void capture_next()
    {
        if (session != nullptr) {
            capture_ext();
        } else {
            capture_screencopy();
        }
    }

    // --- ext-image-copy-capture-v1 ---

    void start_ext(wl_pointer* pointer)
    {
        source = ext_output_image_capture_source_manager_v1_create_source(protocols.output_sources, output);
        const std::uint32_t options =
            pointer != nullptr ? 0U
                               : static_cast<std::uint32_t>(EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
        session = ext_image_copy_capture_manager_v1_create_session(protocols.image_copy, source, options);
        constraints.on_done = [this] { capture_ext(); };
        constraints.on_stopped = [this] { fail("the compositor stopped the capture session"); };
        ext_image_copy_capture_session_v1_add_listener(session, &session_listener, &constraints);
        if (pointer != nullptr) {
            cursor = std::make_unique<Cursor>(this, ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
                                                        protocols.image_copy, source, pointer));
        }
        connection->flush();
    }

    void capture_ext()
    {
        if (closed || ext_frame != nullptr || !constraints.current) {
            return;
        }
        const auto& c = *constraints.current;
        const auto format = c.format();
        if (!format) {
            fail("the capture session offers no shared-memory format farland reads");
            return;
        }
        if (std::pair(c.width, c.height) != size) {
            log::debug(log_component, "{}: captures of {}x{}", name, c.width, c.height);
            force_full = true;
        }
        Buffer* buffer = free_buffer(c.width, c.height, c.width * 4, *format);
        if (buffer == nullptr) {
            return;  // one comes back with release_frame() or take_frame()
        }
        ext_frame = ext_image_copy_capture_session_v1_create_frame(session);
        static constexpr ext_image_copy_capture_frame_v1_listener listener{
            .transform = [](void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/, std::uint32_t /*transform*/) {},
            .damage =
                [](void* data, ext_image_copy_capture_frame_v1* /*frame*/, std::int32_t x, std::int32_t y,
                   std::int32_t width,
                   std::int32_t height) { static_cast<Impl*>(data)->frame_damage.add(Rect{x, y, width, height}); },
            .presentation_time = [](void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/, std::uint32_t /*hi*/,
                                    std::uint32_t /*lo*/, std::uint32_t /*nsec*/) {},
            .ready =
                [](void* data, ext_image_copy_capture_frame_v1* /*frame*/) {
                    auto* self = static_cast<Impl*>(data);
                    ext_image_copy_capture_frame_v1_destroy(self->ext_frame);
                    self->ext_frame = nullptr;
                    self->ready();
                },
            .failed =
                [](void* data, ext_image_copy_capture_frame_v1* /*frame*/, std::uint32_t reason) {
                    auto* self = static_cast<Impl*>(data);
                    ext_image_copy_capture_frame_v1_destroy(self->ext_frame);
                    self->ext_frame = nullptr;
                    self->ext_failed(reason);
                },
        };
        ext_image_copy_capture_frame_v1_add_listener(ext_frame, &listener, this);
        ext_image_copy_capture_frame_v1_attach_buffer(ext_frame, buffer->shm->buffer());
        if (buffer->fresh || buffer->stale.all()) {
            ext_image_copy_capture_frame_v1_damage_buffer(ext_frame, 0, 0, static_cast<std::int32_t>(c.width),
                                                          static_cast<std::int32_t>(c.height));
        } else {
            for (const auto& r : buffer->stale.rects()) {
                ext_image_copy_capture_frame_v1_damage_buffer(ext_frame, r.x, r.y, r.width, r.height);
            }
        }
        ext_image_copy_capture_frame_v1_capture(ext_frame);
        start_capture(buffer);
    }

    void ext_failed(std::uint32_t reason)
    {
        abandon_capture();
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
            fail("the compositor stopped the capture session");
            return;
        }
        if (++failures >= max_failures) {
            fail("captures keep failing");
            return;
        }
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
            // New constraints are on their way unless they came first.
            log::debug(log_component, "{}: capture buffer outdated", name);
        }
        capture_ext();
    }

    // --- wlr-screencopy-unstable-v1 ---

    void capture_screencopy()
    {
        if (closed || screencopy_frame != nullptr) {
            return;
        }
        // The cursor is part of the picture: there is no other way to get it.
        screencopy_frame = zwlr_screencopy_manager_v1_capture_output(protocols.screencopy, 1, output);
        screencopy_buffer.reset();
        y_invert = false;
        static constexpr zwlr_screencopy_frame_v1_listener listener{
            .buffer =
                [](void* data, zwlr_screencopy_frame_v1* frame, std::uint32_t format, std::uint32_t width,
                   std::uint32_t height, std::uint32_t stride) {
                    auto* self = static_cast<Impl*>(data);
                    if ((format == shm_format::xrgb8888 || format == shm_format::argb8888) &&
                        (!self->screencopy_buffer || format == shm_format::xrgb8888)) {
                        self->screencopy_buffer = ScreencopyBuffer{format, width, height, stride};
                    }
                    if (zwlr_screencopy_frame_v1_get_version(frame) <
                        ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION) {
                        self->copy_screencopy();
                    }
                },
            .flags =
                [](void* data, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t flags) {
                    static_cast<Impl*>(data)->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
                },
            .ready =
                [](void* data, zwlr_screencopy_frame_v1* frame, std::uint32_t /*sec_hi*/, std::uint32_t /*sec_lo*/,
                   std::uint32_t /*nsec*/) {
                    auto* self = static_cast<Impl*>(data);
                    if (self->screencopy_whole) {
                        self->frame_damage.add_all();
                    }
                    zwlr_screencopy_frame_v1_destroy(frame);
                    self->screencopy_frame = nullptr;
                    self->ready();
                },
            .failed =
                [](void* data, zwlr_screencopy_frame_v1* frame) {
                    auto* self = static_cast<Impl*>(data);
                    zwlr_screencopy_frame_v1_destroy(frame);
                    self->screencopy_frame = nullptr;
                    self->abandon_capture();
                    log::debug(log_component, "{}: screen copy failed", self->name);
                    if (++self->failures >= max_failures) {
                        self->fail("screen copies keep failing");
                        return;
                    }
                    self->capture_screencopy();
                },
            .damage =
                [](void* data, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t x, std::uint32_t y,
                   std::uint32_t width, std::uint32_t height) {
                    static_cast<Impl*>(data)->frame_damage.add(
                        Rect{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                             static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
                },
            .linux_dmabuf = [](void* /*data*/, zwlr_screencopy_frame_v1* /*frame*/, std::uint32_t /*format*/,
                               std::uint32_t /*width*/, std::uint32_t /*height*/) {},
            .buffer_done = [](void* data,
                              zwlr_screencopy_frame_v1* /*frame*/) { static_cast<Impl*>(data)->copy_screencopy(); },
        };
        zwlr_screencopy_frame_v1_add_listener(screencopy_frame, &listener, this);
        connection->flush();
    }

    void copy_screencopy()
    {
        if (screencopy_frame == nullptr || capturing != nullptr) {
            return;
        }
        if (!screencopy_buffer) {
            fail("the screen copy offers no shared-memory format farland reads");
            return;
        }
        const auto& b = *screencopy_buffer;
        if (std::pair(b.width, b.height) != size) {
            log::debug(log_component, "{}: screen copies of {}x{}", name, b.width, b.height);
            force_full = true;
        }
        Buffer* buffer = free_buffer(b.width, b.height, b.stride, b.format);
        if (buffer == nullptr) {
            // Every buffer is in use; ask again once one comes back.
            zwlr_screencopy_frame_v1_destroy(screencopy_frame);
            screencopy_frame = nullptr;
            return;
        }
        // copy_with_damage waits for the output to change, copy takes the
        // next frame the compositor draws: the first one and one of a new
        // size must come even when nothing on the output moves.
        screencopy_whole = force_full || zwlr_screencopy_frame_v1_get_version(screencopy_frame) <
                                             ZWLR_SCREENCOPY_FRAME_V1_COPY_WITH_DAMAGE_SINCE_VERSION;
        if (screencopy_whole) {
            zwlr_screencopy_frame_v1_copy(screencopy_frame, buffer->shm->buffer());
        } else {
            zwlr_screencopy_frame_v1_copy_with_damage(screencopy_frame, buffer->shm->buffer());
        }
        start_capture(buffer);
    }

    // --- common ---

    void start_capture(Buffer* buffer)
    {
        buffer->state = Buffer::State::capturing;
        capturing = buffer;
        frame_damage.clear();
        connection->flush();
    }

    void abandon_capture()
    {
        if (capturing != nullptr) {
            capturing->state = Buffer::State::free;
            capturing->fresh = true;
            capturing = nullptr;
        }
    }

    void ready()
    {
        Buffer* buffer = std::exchange(capturing, nullptr);
        if (buffer == nullptr) {
            return;
        }
        failures = 0;
        if (y_invert) {
            flip_rows(buffer->shm->data(), buffer->shm->stride(), buffer->shm->height());
        }
        buffer->fresh = false;
        buffer->stale.clear();
        for (auto& other : buffers) {
            if (other.get() != buffer) {
                other->stale.merge(frame_damage);
            }
        }
        Damage damage = frame_damage;
        if (force_full) {
            damage.add_all();
            force_full = false;
        }
        if (pending != nullptr) {
            // Never taken: its changes go with the newer frame.
            pending->state = Buffer::State::free;
            damage.merge(pending_damage);
        }
        buffer->state = Buffer::State::pending;
        pending = buffer;
        pending_damage = std::move(damage);
        size = {buffer->shm->width(), buffer->shm->height()};
        frame_wake.signal();
        capture_next();
    }

    std::optional<Frame> take_frame()
    {
        if (!closed) {
            frame_wake.clear();
        }
        if (pending == nullptr) {
            return std::nullopt;
        }
        if (held != nullptr) {
            held->state = Buffer::State::free;
        }
        held = std::exchange(pending, nullptr);
        held->state = Buffer::State::held;
        const auto& shm = *held->shm;
        Frame frame;
        frame.image = codec::ImageView{shm.data(), shm.width(), shm.height(), shm.stride()};
        frame.damage = pending_damage.for_frame();
        frame.sequence = ++sequence;
        pending_damage.clear();
        capture_next();
        return frame;
    }

    void release_frame()
    {
        if (held != nullptr) {
            held->state = Buffer::State::free;
            held = nullptr;
            capture_next();
        }
    }
};

Result<std::unique_ptr<OutputCapture>> OutputCapture::create(wayland::Connection& connection, wl_output* output,
                                                             wl_pointer* pointer, const Protocols& protocols,
                                                             std::string name)
{
    const bool ext = protocols.image_copy != nullptr && protocols.output_sources != nullptr;
    if (protocols.shm == nullptr || (!ext && protocols.screencopy == nullptr)) {
        return fail(Errc::unsupported, "the compositor has no screen capture protocol");
    }
    auto impl = std::make_unique<Impl>();
    impl->connection = &connection;
    impl->output = output;
    impl->protocols = protocols;
    impl->name = std::move(name);
    if (impl->frame_wake.get() < 0) {
        return fail(Errc::io, "cannot create an eventfd");
    }
    if (ext) {
        impl->start_ext(pointer);
    } else {
        impl->capture_screencopy();
    }
    return std::unique_ptr<OutputCapture>(new OutputCapture(std::move(impl)));
}

OutputCapture::OutputCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

OutputCapture::~OutputCapture() = default;

FrameSource& OutputCapture::frames() noexcept
{
    return impl_->frames;
}

CursorSource* OutputCapture::cursor() noexcept
{
    return impl_->cursor.get();
}

bool OutputCapture::closed() const noexcept
{
    return impl_->closed;
}

const std::string& OutputCapture::error() const noexcept
{
    return impl_->error;
}

std::string_view OutputCapture::protocol() const noexcept
{
    return impl_->session != nullptr ? "ext-image-copy-capture-v1" : "wlr-screencopy-unstable-v1";
}

}  // namespace farland::platform::wlroots
